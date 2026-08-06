// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Rendering/RenderCullVolume.h"


bool AAutomataOrchestrator::GetCameraView(FVector& OutLocation, FVector& OutForward) const
{
	if (!GamePC || !GamePC->PlayerCameraManager)
	{
		return false;
	}

	OutLocation = GamePC->PlayerCameraManager->GetCameraLocation();
	OutForward = GamePC->PlayerCameraManager->GetCameraRotation().Vector();
	return true;
}

bool AAutomataOrchestrator::ComputeAliveCellsBounds(FVector& OutCenter, float& OutRadius) const
{
	if (!Grid || Grid->Num() == 0)
	{
		return false;
	}

	TArray<FIntVector> AliveCells;
	Grid->GetAliveCells(AliveCells);
	return ComputeCellsBounds(AliveCells, OutCenter, OutRadius);
}

bool AAutomataOrchestrator::ComputeVisibleCellsBounds(FVector& OutCenter, float& OutRadius)
{
	if (!Grid || Grid->Num() == 0)
	{
		return false;
	}

	// Те же три фильтра, что BuildCellRenderData() применяет к живым клеткам -
	// продублировано намеренно, см. doc-comment в заголовке про то, почему не
	// вызывается сама BuildCellRenderData().
	TArray<FIntVector> Cells;
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	if (CullVolume)
	{
		Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), Cells);
	}
	else
	{
		Grid->GetAliveCells(Cells);
	}

	FVector SliceOrigin = FVector::ZeroVector;
	FVector SliceForward = FVector::ForwardVector;
	const bool bSliceActive = bEnableViewSlice && GetCameraView(SliceOrigin, SliceForward);
	const float SliceMinDepth = ViewSliceDistance - ViewSliceThickness * 0.5f;
	const float SliceMaxDepth = ViewSliceDistance + ViewSliceThickness * 0.5f;

	TArray<bool> AgeFilterMask;
	const bool bAgeFilterActive = BuildAgeFilterMask(AgeFilterMask);

	TArray<FIntVector> VisibleCells;
	VisibleCells.Reserve(Cells.Num());
	for (const FIntVector& Cell : Cells)
	{
		if (bAgeFilterActive && !AgeFilterMask[Grid->GetAge(Cell)])
		{
			continue;
		}

		if (bSliceActive)
		{
			const FVector World = Grid->GridToWorld(Cell);
			const double Depth = FVector::DotProduct(World - SliceOrigin, SliceForward);
			if (Depth < SliceMinDepth || Depth > SliceMaxDepth)
			{
				continue;
			}
		}

		VisibleCells.Add(Cell);
	}

	return ComputeCellsBounds(VisibleCells, OutCenter, OutRadius);
}

bool AAutomataOrchestrator::ComputeCellsBounds(const TArray<FIntVector>& AliveCells, FVector& OutCenter, float& OutRadius) const
{
	if (!Grid || AliveCells.Num() == 0)
	{
		return false;
	}

	// Приближённая МИНИМАЛЬНАЯ описанная сфера (алгоритм Ritter'а), а не
	// прежняя "сфера вокруг углов AABB" (центр AABB, радиус - половина его
	// диагонали): для формы, не достающей до углов своего параллелепипеда
	// (типичный случай - растущая структура автомата обычно скорее
	// округлая/гранёная, чем буквально кубическая, особенно по мере роста),
	// сфера вокруг углов AABB завышает нужный радиус до sqrt(3)≈1.73x - и
	// чем "органичнее"/крупнее становится форма, тем сильнее рос этот запас,
	// из-за чего Home/старое авто-кадрирование R отъезжали заметно дальше
	// необходимого (наблюдалось на практике - см. скриншоты в обсуждении).
	// Алгоритм Ritter'а по-прежнему строгая ВЕРХНЯЯ граница (гарантированно
	// включает КАЖДУЮ живую клетку, как и раньше - контракт SelectCellUnderCursor()'s
	// MaxDistance ниже не нарушен), просто заметно теснее для типичных форм.
	// GridToWorld() пересчитывается по требованию из AliveCells через
	// локальную лямбду, а не кэшируется во второй TArray<FVector> - на 7M+
	// живых клеток это удвоило бы временное выделение памяти ради дешёвого
	// умножения, которое и так стоит копейки.
	auto WorldPos = [this, &AliveCells](int32 Index) { return Grid->GridToWorld(AliveCells[Index]); };

	// Шаг 1: от произвольной точки (первая) ищем самую дальнюю (P1), затем
	// от P1 - самую дальнюю (P2). Пара (P1, P2) - хорошее приближение самой
	// длинной оси облака точек, классическое начало алгоритма Ritter'а.
	int32 IndexP1 = 0;
	{
		double BestDistSq = -1.0;
		const FVector P0 = WorldPos(0);
		for (int32 Index = 0; Index < AliveCells.Num(); ++Index)
		{
			const double DistSq = FVector::DistSquared(P0, WorldPos(Index));
			if (DistSq > BestDistSq)
			{
				BestDistSq = DistSq;
				IndexP1 = Index;
			}
		}
	}

	int32 IndexP2 = 0;
	{
		double BestDistSq = -1.0;
		const FVector P1 = WorldPos(IndexP1);
		for (int32 Index = 0; Index < AliveCells.Num(); ++Index)
		{
			const double DistSq = FVector::DistSquared(P1, WorldPos(Index));
			if (DistSq > BestDistSq)
			{
				BestDistSq = DistSq;
				IndexP2 = Index;
			}
		}
	}

	FVector Center = (WorldPos(IndexP1) + WorldPos(IndexP2)) * 0.5;
	double Radius = FVector::Dist(WorldPos(IndexP1), WorldPos(IndexP2)) * 0.5;

	// Шаг 2: расширяем стартовую сферу, чтобы включить каждую оставшуюся
	// точку - классическое инкрементальное расширение Ritter'а (сдвигаем
	// центр к точке-нарушителю ровно настолько, чтобы она оказалась на
	// новой границе сферы).
	for (int32 Index = 0; Index < AliveCells.Num(); ++Index)
	{
		const FVector Position = WorldPos(Index);
		const double Dist = FVector::Dist(Center, Position);
		if (Dist > Radius)
		{
			const double NewRadius = (Radius + Dist) * 0.5;
			const double Ratio = (NewRadius - Radius) / Dist;
			Center += (Position - Center) * Ratio;
			Radius = NewRadius;
		}
	}

	OutCenter = Center;
	// Запас на полклетки - GridToWorld() даёт координаты ЦЕНТРА клетки, а не
	// её края. Считается от НАРИСОВАННОГО габарита (шаг решётки, умноженный
	// на CellMeshScaleMultiplier), а не от одного шага: на подрешётке (ГЦК,
	// ОЦК) заселён каждый второй узел, ячейка Вороного там вдвое крупнее
	// шага, и прежний запас в полшага занижал радиус ровно вдвое - кадр по
	// Home подрезал крайние клетки. Максимум по осям - потому что запас
	// добавляется к радиусу СФЕРЫ, и по вытянутой оси он должен покрывать
	// самый крупный габарит.
	const double HalfCellWorldSize = Grid->GetLattice().GetMaxCellWorldExtent() * 0.5 * FMath::Max(1.0f, CellMeshScaleMultiplier);
	OutRadius = static_cast<float>(Radius + HalfCellWorldSize);

	return true;
}
