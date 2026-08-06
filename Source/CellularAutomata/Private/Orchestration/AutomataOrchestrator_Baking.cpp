// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Grid/GridDownsample.h"
#include "Automata/Meshing/CellMeshBuilder.h"
#include "ProceduralMeshComponent.h"


void AAutomataOrchestrator::EnsureBakedMeshComponent()
{
	if (BakedMeshComponent)
	{
		return;
	}

	BakedMeshComponent = NewObject<UProceduralMeshComponent>(this);
	BakedMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BakedMeshComponent->SetupAttachment(CellsMeshHierarchical);
	BakedMeshComponent->RegisterComponent();
}

void AAutomataOrchestrator::ClearBakedMesh()
{
	if (BakedMeshComponent)
	{
		BakedMeshComponent->ClearAllMeshSections();
	}
}

void AAutomataOrchestrator::BakeCellsToMesh()
{
	// Мы освобождаем Grid ниже - фоновый шаг (Next()/StepAsync()) в этот
	// момент его читает, тот же guard, что у всех путей замены Grid.
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("BakeCellsToMesh: фоновый шаг ещё считается - подождите его завершения"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("BakeCellsToMesh: сетка не инициализирована"));
		return;
	}

	// Запекание отсекает грани по ШЕСТИ ОСЕВЫМ соседям (CellMeshBuilder.cpp,
	// таблица GFaces) - то есть предполагает, что клетка касается соседей
	// именно по осям и что все они принадлежат тому же набору. На любой
	// подрешётке это неверно в самой основе: сосед Cell+(1,0,0) там НИКОГДА не
	// жив, потому что у него другая чётность, поэтому не отсекается ни одна
	// грань. На выходе получается 6*N граней россыпью отдельных кубиков вместо
	// цельной оболочки - и вшестеро больше собственной оценки бюджета.
	//
	// Отказ здесь - осознанная СМЕНА ПОВЕДЕНИЯ: раньше на ГЦК/ОЦК запекание
	// "работало" в том смысле, что не падало, но результат был мусорным уже
	// тогда. Молчаливый мусор, съедающий гигабайты, хуже честного отказа.
	if (GenerationParams.ParityFilter != ECellParityFilter::None)
	{
		UE_LOG(LogTemp, Warning, TEXT("BakeCellsToMesh: запекание не поддерживает подрешётку (ParityFilter=%d) - отсечение граней идёт по 6 осевым соседям, которых на ней нет"),
			static_cast<int32>(GenerationParams.ParityFilter));
		ShowStatusMessage(StatusKey_Bake, TEXT("[M] Запекание работает только на простой кубической решётке (форма \"Куб\")"));
		return;
	}

	// Снимок несовместим с продолжением симуляции (сетки после него уже
	// нет) - останавливаем и Play, и автошаг Shift+F, если шли.
	if (bSimulationRunning)
	{
		Stop();
	}
	if (bFastStepActive)
	{
		StopFastStep();
	}

	// Есть активное выделение - запекаем только его (отфильтрованное до
	// реально живых, как в StartFromSelection()); иначе все живые клетки.
	TArray<FIntVector> CellsToBake;
	if (SelectedCells.Num() > 0)
	{
		CellsToBake.Reserve(SelectedCells.Num());
		for (const FIntVector& Cell : SelectedCells)
		{
			if (Grid->IsAlive(Cell))
			{
				CellsToBake.Add(Cell);
			}
		}
	}
	else
	{
		Grid->GetAliveCells(CellsToBake);
	}

	if (CellsToBake.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("BakeCellsToMesh: нечего запекать - нет ни выделенных, ни живых клеток"));
		ShowStatusMessage(StatusKey_Bake, TEXT("[M] Нечего запекать - нет ни выделенных, ни живых клеток"));
		return;
	}

	// Без выделения печём всю сетку, а значит принадлежность соседа набору
	// можно спрашивать прямо у неё, не строя TSet по миллионам клеток (см.
	// CellMeshBuilder::BuildFromCells()). Ответ тот же, памяти на сотни
	// мегабайт меньше.
	const bool bUseGridMembership = SelectedCells.Num() == 0;

	// Свободная физическая память - чтобы бюджет можно было ставить осознанно,
	// а не наугад: оценка сама по себе не говорит, много это или мало на
	// конкретной машине.
	const double AvailableMB = double(FPlatformMemory::GetStats().AvailablePhysical) / (1024.0 * 1024.0);

	// Подбор огрубления. Считаем точное число наружных граней (без единой
	// аллокации под геометрию), и если оценка не влезает в бюджет - сливаем
	// K x K x K клеток в одну и пробуем снова. Выигрыш двойной: клеток в K^3
	// раз меньше, и структура плотнее, отчего падает ещё и число граней НА
	// клетку. Без этого нажатие M на большой пористой сетке съедало всю
	// память и вешало редактор - наблюдалось.
	//
	// Огрублённая сетка сама себе набор, поэтому принадлежность соседа
	// спрашивается у неё (bUseGridMembership = true) независимо от того,
	// печём мы выделение или всё: выделение уже учтено при её построении.
	const double CountStartSeconds = FPlatformTime::Seconds();
	TUniquePtr<FDenseCellGrid> CoarseGrid;
	TArray<FIntVector> CoarseCells;
	int32 Simplification = 1;
	int64 ExposedFaceCount = CellMeshBuilder::CountExposedFaces(*Grid, CellsToBake, bUseGridMembership);
	double EstimatedMB = double(CellMeshBuilder::EstimateMeshBytes(ExposedFaceCount)) / (1024.0 * 1024.0);

	while (EstimatedMB > double(BakeMemoryBudgetMB) && bAutoSimplifyBake && Simplification < MaxBakeSimplification)
	{
		Simplification *= 2;
		CoarseGrid = GridDownsample::Downsample(*Grid, CellsToBake, Simplification, ChunkSize);
		CoarseGrid->GetAliveCells(CoarseCells);
		ExposedFaceCount = CellMeshBuilder::CountExposedFaces(*CoarseGrid, CoarseCells, /*bUseGridMembership=*/true);
		EstimatedMB = double(CellMeshBuilder::EstimateMeshBytes(ExposedFaceCount)) / (1024.0 * 1024.0);

		UE_LOG(LogTemp, Log, TEXT("BakeCellsToMesh: огрубление x%d -> клеток %d, граней %lld, оценка ~%.0f МБ"),
			Simplification, CoarseCells.Num(), ExposedFaceCount, EstimatedMB);
	}

	const double CountSeconds = FPlatformTime::Seconds() - CountStartSeconds;

	UE_LOG(LogTemp, Log, TEXT("BakeCellsToMesh: клеток %d -> наружных граней %lld, оценка пика ~%.0f МБ, огрубление x%d (подбор: %.0f мс, бюджет %d МБ, свободно %.0f МБ)"),
		CellsToBake.Num(), ExposedFaceCount, EstimatedMB, Simplification, CountSeconds * 1000.0, BakeMemoryBudgetMB, AvailableMB);

	if (EstimatedMB > double(BakeMemoryBudgetMB))
	{
		UE_LOG(LogTemp, Warning, TEXT("BakeCellsToMesh: отказ - потребуется ~%.0f МБ при бюджете %d МБ даже с огрублением x%d"),
			EstimatedMB, BakeMemoryBudgetMB, Simplification);
		ShowStatusMessage(StatusKey_Bake, FString::Printf(
			TEXT("[M] Бейк отменён: нужно ~%.0f МБ при бюджете %d МБ (свободно %.0f МБ) даже с огрублением x%d.  Выделите кусок или поднимите BakeMemoryBudgetMB / MaxBakeSimplification"),
			EstimatedMB, BakeMemoryBudgetMB, AvailableMB, Simplification));
		return;
	}

	UMaterialInterface* MeshMaterial = BakedMeshMaterial;
	if (!MeshMaterial)
	{
		// Фолбэка больше нет - причина та же, что и в RefreshGhostShape().
		UE_LOG(LogTemp, Warning, TEXT("BakeCellsToMesh: BakedMeshMaterial не назначен - меш будет нарисован дефолтным материалом движка"));
	}

	const double BakeStartSeconds = FPlatformTime::Seconds();
	CellMeshBuilder::FCellMeshData MeshData = CoarseGrid
		? CellMeshBuilder::BuildFromCells(*CoarseGrid, CoarseCells, /*bUseGridMembership=*/true)
		: CellMeshBuilder::BuildFromCells(*Grid, CellsToBake, bUseGridMembership);
	const double BuildSeconds = FPlatformTime::Seconds() - BakeStartSeconds;

	// Огрублённая копия больше не нужна - освобождаем до того, как отдадим
	// геометрию компоненту, чтобы она не сидела в памяти во время самого
	// затратного момента (там живут ОБЕ копии геометрии, см.
	// CellMeshBuilder::EstimateMeshBytes()).
	CoarseCells.Empty();
	CoarseGrid.Reset();

	EnsureBakedMeshComponent();
	BakedMeshComponent->ClearAllMeshSections();
	const double SectionStartSeconds = FPlatformTime::Seconds();
	BakedMeshComponent->CreateMeshSection_LinearColor(0, MeshData.Vertices, MeshData.Triangles, MeshData.Normals, MeshData.UVs,
		TArray<FLinearColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision=*/false);
	if (MeshMaterial)
	{
		BakedMeshComponent->SetMaterial(0, MeshMaterial);
	}
	const double SectionSeconds = FPlatformTime::Seconds() - SectionStartSeconds;

	// Выгрузка: инстансы всех клеточных компонентов и сама сетка. Флаги
	// чанкового разлива сбрасываем тоже - иначе недоигранный разлив
	// (AdvanceChunkedRender() в Tick()) досыпал бы инстансы обратно уже
	// ПОСЛЕ очистки.
	if (SelectionMeshComponent)
	{
		SelectionMeshComponent->ClearInstances();
	}
	if (UInstancedStaticMeshComponent* BaseComponent = GetActiveCellsMeshComponent())
	{
		BaseComponent->ClearInstances();
	}
	ClearInactiveCellsMeshComponent();
	bChunkedRenderInProgress = false;
	SelectedCells.Reset();
	// InitialStateCells намеренно НЕ трогаем - R после осмотра снимка
	// вернёт извлечённый паттерн, если он был (см. ResetToInitialState()).
	Grid.Reset();

	UE_LOG(LogTemp, Log, TEXT("BakeCellsToMesh: %d клеток -> %d вершин / %d треугольников (геометрия: %.2f мс, секция: %.2f мс); сетка и инстансы выгружены, R начнёт новый прогон"),
		CellsToBake.Num(), MeshData.Vertices.Num(), MeshData.Triangles.Num() / 3, BuildSeconds * 1000.0, SectionSeconds * 1000.0);
	ShowStatusMessage(StatusKey_Bake, FString::Printf(
		TEXT("[M] Запечено: %d клеток -> %d треугольников, ~%.0f МБ за %.1f с%s.  Сетка выгружена, R начнёт заново"),
		CellsToBake.Num(), MeshData.Triangles.Num() / 3, EstimatedMB, (CountSeconds + BuildSeconds + SectionSeconds),
		Simplification > 1 ? *FString::Printf(TEXT(", огрубление x%d"), Simplification) : TEXT("")));
}
