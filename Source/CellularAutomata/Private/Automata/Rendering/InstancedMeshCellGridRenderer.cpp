#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"

#include "Automata/Grid/CellGrid.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Algo/Sort.h"
#include "Algo/Reverse.h"
#include "Algo/RandomShuffle.h"

FInstancedMeshCellGridRenderer::FInstancedMeshCellGridRenderer(UInstancedStaticMeshComponent* InComponent)
	: Component(InComponent)
{
}

void FInstancedMeshCellGridRenderer::SetMesh(UStaticMesh* InMesh)
{
	Mesh = InMesh;
}

void FInstancedMeshCellGridRenderer::SetMaterial(UMaterialInterface* InMaterial)
{
	Material = InMaterial;
}

void FInstancedMeshCellGridRenderer::SetScaleMultiplier(float InScaleMultiplier)
{
	ScaleMultiplier = InScaleMultiplier;
}

void FInstancedMeshCellGridRenderer::Render(const FCellGrid& Grid)
{
	// Order/CameraLocation не влияют на однократный Render() - весь массив
	// всё равно уходит одним AddInstances() внутри одного и того же кадра,
	// порядок элементов внутри него не наблюдаем.
	BeginRender(Grid, EChunkedRenderOrder::Sequential, FVector::ZeroVector);
	// Без ограничения на размер чанка - весь PendingTransforms уходит одним
	// вызовом AddInstances(), как и раньше до появления чанкинга; цикл
	// формален (тела достаточно ровно одной итерации), но так BeginRender()/
	// AdvanceRenderChunk() остаются единственным местом с этой логикой.
	while (AdvanceRenderChunk(TNumericLimits<int32>::Max()))
	{
	}
}

void FInstancedMeshCellGridRenderer::BeginRender(const FCellGrid& Grid, EChunkedRenderOrder Order, const FVector& CameraLocation)
{
	PendingTransforms.Reset();
	PendingCursor = 0;
	LastTimings = FRenderTimings();

	UInstancedStaticMeshComponent* Comp = Component.Get();
	if (!Comp)
	{
		UE_LOG(LogTemp, Warning, TEXT("FInstancedMeshCellGridRenderer: component is invalid"));
		return;
	}

	const double SetMeshStartSeconds = FPlatformTime::Seconds();
	if (Mesh.IsValid())
	{
		Comp->SetStaticMesh(Mesh.Get());
	}
	if (Material.IsValid())
	{
		Comp->SetMaterial(0, Material.Get());
	}
	LastTimings.SetMeshSeconds = FPlatformTime::Seconds() - SetMeshStartSeconds;

	const double ClearStartSeconds = FPlatformTime::Seconds();
	Comp->ClearInstances();
	LastTimings.ClearSeconds = FPlatformTime::Seconds() - ClearStartSeconds;

	// Масштабируем меш так, чтобы его bounding box совпадал с размером клетки -
	// иначе при несовпадении реального размера меша и CellSize сетка получается
	// неровной (щели или наложение соседних кубов).
	const double ScaleStartSeconds = FPlatformTime::Seconds();
	FVector InstanceScale = FVector::OneVector;
	if (const UStaticMesh* MeshPtr = Mesh.Get())
	{
		const FVector MeshSize = MeshPtr->GetBounds().BoxExtent * 2.0;
		if (!MeshSize.IsNearlyZero())
		{
			InstanceScale = FVector(Grid.GetCellSize()) / MeshSize;
		}
	}
	// Поверх точной подгонки под CellSize - см. SetScaleMultiplier() (1.0
	// для обычных клеток, чуть больше для подсветки выделения).
	InstanceScale *= ScaleMultiplier;
	LastTimings.ScaleSeconds = FPlatformTime::Seconds() - ScaleStartSeconds;

	const double GetAliveStartSeconds = FPlatformTime::Seconds();
	TArray<FIntVector> AliveCells;
	Grid.GetAliveCells(AliveCells);
	LastTimings.GetAliveSeconds = FPlatformTime::Seconds() - GetAliveStartSeconds;

	// Переупорядочиваем AliveCells (а не готовые FTransform - FIntVector
	// втрое меньше по размеру) до нарезки на чанки: результат определяет, в
	// каком порядке клетки появляются по кадрам "разлитого" реавила (см.
	// EChunkedRenderOrder). Для одноразового Render() (Order всегда
	// Sequential) это no-op с нулевой стоимостью.
	const double ReorderStartSeconds = FPlatformTime::Seconds();
	switch (Order)
	{
	case EChunkedRenderOrder::Sequential:
		break;

	case EChunkedRenderOrder::SequentialReversed:
		Algo::Reverse(AliveCells);
		break;

	case EChunkedRenderOrder::Shuffled:
		Algo::RandomShuffle(AliveCells);
		break;

	case EChunkedRenderOrder::DistanceFromCameraNearFirst:
		Algo::Sort(AliveCells, [&Grid, &CameraLocation](const FIntVector& A, const FIntVector& B)
		{
			return FVector::DistSquared(Grid.GridToWorld(A), CameraLocation) < FVector::DistSquared(Grid.GridToWorld(B), CameraLocation);
		});
		break;

	case EChunkedRenderOrder::DistanceFromCameraFarFirst:
		Algo::Sort(AliveCells, [&Grid, &CameraLocation](const FIntVector& A, const FIntVector& B)
		{
			return FVector::DistSquared(Grid.GridToWorld(A), CameraLocation) > FVector::DistSquared(Grid.GridToWorld(B), CameraLocation);
		});
		break;

	case EChunkedRenderOrder::FromCenterOutward:
		if (AliveCells.Num() > 0)
		{
			FIntVector MinCell = AliveCells[0];
			FIntVector MaxCell = AliveCells[0];
			for (const FIntVector& Cell : AliveCells)
			{
				MinCell.X = FMath::Min(MinCell.X, Cell.X);
				MinCell.Y = FMath::Min(MinCell.Y, Cell.Y);
				MinCell.Z = FMath::Min(MinCell.Z, Cell.Z);
				MaxCell.X = FMath::Max(MaxCell.X, Cell.X);
				MaxCell.Y = FMath::Max(MaxCell.Y, Cell.Y);
				MaxCell.Z = FMath::Max(MaxCell.Z, Cell.Z);
			}
			const FVector Center = (Grid.GridToWorld(MinCell) + Grid.GridToWorld(MaxCell)) * 0.5;
			Algo::Sort(AliveCells, [&Grid, &Center](const FIntVector& A, const FIntVector& B)
			{
				return FVector::DistSquared(Grid.GridToWorld(A), Center) < FVector::DistSquared(Grid.GridToWorld(B), Center);
			});
		}
		break;
	}
	LastTimings.ReorderSeconds = FPlatformTime::Seconds() - ReorderStartSeconds;

	// AddInstance() по одному элементу пересобирает внутренний буфер инстансов
	// на каждый вызов (супралинейный рост при большом числе клеток) - строим
	// все трансформы разом, а добавляем их через AddInstances() в
	// AdvanceRenderChunk() (одним батчем целиком или по частям).
	const double BuildTransformsStartSeconds = FPlatformTime::Seconds();
	PendingTransforms.Reserve(AliveCells.Num());
	for (const FIntVector& Cell : AliveCells)
	{
		PendingTransforms.Add(FTransform(FQuat::Identity, Grid.GridToWorld(Cell), InstanceScale));
	}
	LastTimings.BuildTransformsSeconds = FPlatformTime::Seconds() - BuildTransformsStartSeconds;

	// Логирование намеренно НЕ здесь: UE_LOG сам по себе (форматирование +
	// запись в файл) стоит времени, и если логировать внутри BeginRender(),
	// эта стоимость попадает в измеряемый снаружи интервал (см. Next()/
	// GenerateRandom() в AutomataOrchestrator), но не в одну из полей
	// LastTimings - разница выглядит как необъяснённый пробел в замерах.
	// Вызывающая сторона логирует один раз, объединяя это с шагом симуляции.
}

bool FInstancedMeshCellGridRenderer::AdvanceRenderChunk(int32 MaxCellsThisChunk)
{
	UInstancedStaticMeshComponent* Comp = Component.Get();
	if (!Comp)
	{
		PendingTransforms.Reset();
		PendingCursor = 0;
		return false;
	}

	const int32 Remaining = PendingTransforms.Num() - PendingCursor;
	if (Remaining <= 0)
	{
		return false;
	}

	const int32 CountThisChunk = FMath::Min(MaxCellsThisChunk, Remaining);

	TArray<FTransform> ChunkTransforms;
	ChunkTransforms.Append(PendingTransforms.GetData() + PendingCursor, CountThisChunk);

	// bUpdateNavigation=false: навмеш клеткам автомата не нужен.
	const double AddInstanceStartSeconds = FPlatformTime::Seconds();
	Comp->AddInstances(ChunkTransforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false, /*bUpdateNavigation=*/false);
	LastTimings.AddInstanceSeconds += FPlatformTime::Seconds() - AddInstanceStartSeconds;

	PendingCursor += CountThisChunk;
	return PendingCursor < PendingTransforms.Num();
}
