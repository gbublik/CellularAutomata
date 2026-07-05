// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "Automata/Rendering/FilteredCellGridView.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/CellAging.h"
#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"
#include "Automata/Simulation/ComputeStrategy/GpuComputeStrategy.h"
#include "Automata/Selection/CellSelection.h"
#include "Async/Async.h"


// Sets default values
AAutomataOrchestrator::AAutomataOrchestrator()
{
	// Тик нужен для непрерывной симуляции (Start()/Stop()), но не должен
	// крутиться, пока симуляция не запущена явно.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// Оба компонента для отрисовки клеток создаются всегда - переключение
	// CellMeshComponentType в рантайме (см. GetActiveCellsMeshComponent())
	// просто выбирает, какой из них получает AddInstances/ClearInstances, без
	// пересоздания компонентов (Live Coding не умеет безопасно хот-патчить
	// смену класса CreateDefaultSubobject-компонента на уже существующих в
	// уровне акторах - см. CLAUDE.md). Клетки чисто визуальные, коллизия не
	// нужна и только замедляет добавление инстансов при большом их количестве.
	CellsMeshHierarchical = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("CellsMeshHierarchical"));
	CellsMeshHierarchical->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RootComponent = CellsMeshHierarchical;

	CellsMeshFlat = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CellsMeshFlat"));
	CellsMeshFlat->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CellsMeshFlat->SetupAttachment(CellsMeshHierarchical);
}

// Called when the game starts or when spawned
void AAutomataOrchestrator::BeginPlay()
{
	Super::BeginPlay();
	InitializePlayerController();
	RebuildAgeMeshComponents();
	EnsureSelectionMeshComponent();
	GenerateRandom();
}

// Called every frame
void AAutomataOrchestrator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Разлитый по кадрам рендер (см. bEnableChunkedRender) продолжается
	// независимо от bSimulationRunning - если игру остановили посреди
	// "разлива", он всё равно должен доехать до конца, а не застрять
	// наполовину отрисованным.
	if (bChunkedRenderInProgress)
	{
		AdvanceChunkedRender();
	}

	// Автошаг Shift+F (см. StartFastStep()) - взаимоисключающ с
	// bSimulationRunning (Start() отказывает, пока это активно, и наоборот),
	// поэтому безопасно делить TimeSinceLastStep с обычным Play.
	// Пока включён "ждать разлив" (см. bWaitForChunkedRenderToFinish), не
	// запускаем следующий шаг, пока предыдущий чанковый "разлив" ещё
	// рисуется - AdvanceChunkedRender() выше в этом же Tick() уже мог его
	// как раз завершить, так что проверка сразу актуальна для этого кадра.
	const bool bBlockedByChunkedRender = bWaitForChunkedRenderToFinish && bChunkedRenderInProgress;

	if (bFastStepActive)
	{
		TimeSinceLastStep += DeltaTime;
		const float StepInterval = 1.0f / FMath::Max(Speed, KINDA_SMALL_NUMBER);

		if (TimeSinceLastStep >= StepInterval && !bStepInProgress && !bBlockedByChunkedRender)
		{
			TimeSinceLastStep = 0.0f;
			StepAsync();
		}

		return;
	}

	if (!bSimulationRunning)
	{
		return;
	}

	// Копим DeltaTime и шагаем симуляцию с интервалом 1/Speed секунд - а не
	// раз в кадр - чтобы Speed действительно означал "шагов в секунду"
	// независимо от FPS, и чтобы правки Speed в Details panel подхватывались
	// немедленно (интервал пересчитывается каждый раз, а не кэшируется).
	TimeSinceLastStep += DeltaTime;
	const float StepInterval = 1.0f / FMath::Max(Speed, KINDA_SMALL_NUMBER);

	// Шаг считается асинхронно (см. StepAsync()) - пока предыдущий фоновый
	// счёт не завершился (bStepInProgress), новый не запускаем (гонка на
	// Grid), просто ждём. Раньше здесь был while-цикл, "нагоняющий"
	// пропущенные шаги за один тик - для синхронного Next() это было
	// безопасно, но для асинхронного шага означало бы запуск нескольких
	// фоновых Step() поверх друг друга. По умолчанию (bWaitForChunkedRenderToFinish
	// == false) рендер (в т.ч. чанковый "разлив") не гейтит следующий шаг
	// вовсе - если он ещё не закончился, когда готово новое поколение,
	// ApplyStepResult() сам его прерывает и перезапускает с нуля на новом
	// состоянии (см. RenderCurrentGrid()/BeginRender()); если же включено,
	// bBlockedByChunkedRender (выше) держит следующий StepAsync() в ожидании,
	// пока разлив не дорисуется сам, и тогда ApplyStepResult() уже не застаёт
	// bChunkedRenderInProgress истинным. Оставшееся время не копится "про
	// запас" - реальная скорость сама упрётся в то, сколько Step() занимает
	// на этой сетке (плюс, в режиме ожидания, во сколько занимает разлив).
	if (TimeSinceLastStep >= StepInterval && !bStepInProgress && !bBlockedByChunkedRender)
	{
		TimeSinceLastStep = 0.0f;
		StepAsync();
	}
}

void AAutomataOrchestrator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Дожидаемся фонового шага (если он в полёте) ДО того, как актор начнёт
	// разрушаться - иначе StepAsync()'s CurrentGridPtr (сырой Grid.Get()) может
	// пережить сам Grid и фоновый ParallelFor разыменует уже освобождённую
	// память (см. PendingStepFuture в заголовке).
	if (PendingStepFuture.IsValid())
	{
		PendingStepFuture.Wait();
	}

	Super::EndPlay(EndPlayReason);
}

void AAutomataOrchestrator::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RebuildAgeMeshComponents();
	EnsureSelectionMeshComponent();
}

#if WITH_EDITOR
void AAutomataOrchestrator::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, AgeMaterials)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellMeshComponentType))
	{
		RebuildAgeMeshComponents();
	}
}
#endif

void AAutomataOrchestrator::NewSeed()
{
	Seed = FMath::Rand();
	GenerateRandom();
}

void AAutomataOrchestrator::AdjustSpeed(float Delta)
{
	// Верхняя граница здесь выше, чем UIMax в UPROPERTY-метаданных Speed
	// (10.0) - тот UIMax только ограничивает слайдер в Details panel, не сам
	// ClampMax, так что хоткеям +/- можно позволить разогнать Speed дальше.
	Speed = FMath::Clamp(Speed + Delta, 0.1f, 100.0f);
	UE_LOG(LogTemp, Log, TEXT("AdjustSpeed: Speed = %.2f"), Speed);
}

void AAutomataOrchestrator::AdjustStepsPerRender(int32 Delta)
{
	StepsPerRender = FMath::Max(StepsPerRender + Delta, 1);
	UE_LOG(LogTemp, Log, TEXT("AdjustStepsPerRender: StepsPerRender = %d"), StepsPerRender);
}

void AAutomataOrchestrator::InitializeHUD()
{	
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		UiController = MakeUnique<FUiController>(PC);
		UiController->SetHUDClass(HUDWidgetClass);
		
		UiController->ShowHUD();
	}
}

void AAutomataOrchestrator::InitializePlayerController()
{
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		GamePC = Cast<AGamePlayerController>(PC);
		if (GamePC)
		{
			GamePC->SetCameraControlEnabled(true);
			UE_LOG(LogTemp, Warning, TEXT("GamePlayerController setup complete"));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Wrong PlayerController class! Using: %s"), *PC->GetClass()->GetName());
		}
	}
}

void AAutomataOrchestrator::PostActorCreated()
{
	Super::PostActorCreated();
	InitializeHUD();
}

UInstancedStaticMeshComponent* AAutomataOrchestrator::GetActiveCellsMeshComponent() const
{
	return (CellMeshComponentType == ECellMeshComponentType::HierarchicalInstanced)
		? static_cast<UInstancedStaticMeshComponent*>(CellsMeshHierarchical)
		: static_cast<UInstancedStaticMeshComponent*>(CellsMeshFlat);
}

void AAutomataOrchestrator::ApplyCellCullDistances()
{
	// Отсечение по расстоянию (не HLOD - см. doc-comment CellCullEndDistance
	// в заголовке) применяем к ОБОИМ компонентам одинаково, а не только к
	// активному - если CellMeshComponentType переключат позже, второй
	// компонент не должен остаться со старыми (или дефолтными) значениями.
	// SetCullDistances() сама no-op, если значения не изменились, так что
	// звать её лишний раз дёшево. Пока bEnableCellCulling == false -
	// применяем (0, 0), не трогая сами CellCullStartDistance/CellCullEndDistance,
	// чтобы выключение хоткеем B не сбрасывало подобранные числа.
	//
	// Отдельная функция (не встроена в рендер): SetCullDistances() обновляет
	// уже существующий SceneProxy на Render Thread немедленно (см.
	// UInstancedStaticMeshComponent::SetCullDistances()) - ему не нужен новый
	// AddInstances()/рендер, чтобы подействовать. SetCellCullingEnabled()
	// зовёт эту функцию сама, сразу, без ожидания следующего рендера.
	const int32 CullStart = bEnableCellCulling ? FMath::Max(0, FMath::RoundToInt(CellCullStartDistance)) : 0;
	const int32 CullEnd = bEnableCellCulling ? FMath::Max(0, FMath::RoundToInt(CellCullEndDistance)) : 0;

	// Логируем только на фактическое изменение (не на каждый вызов) -
	// сверяемся с уже применённым значением на компоненте, а не храним
	// отдельное поле-кэш. Именно это "начало отсечения": сам движок решает,
	// какие конкретно инстансы не рисовать, каждый кадр и без обратной связи
	// в C++ - здесь мы можем зафиксировать только момент, когда порог
	// (Start/End) поменялся, т.е. отсечение включилось/выключилось/сдвинулось.
	int32 PrevCullStart = 0;
	int32 PrevCullEnd = 0;
	CellsMeshHierarchical->GetCullDistances(PrevCullStart, PrevCullEnd);
	if (PrevCullStart != CullStart || PrevCullEnd != CullEnd)
	{
		if (CullEnd > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("ApplyCellCullDistances: отсечение клеток по расстоянию включено (Start=%d, End=%d)"), CullStart, CullEnd);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("ApplyCellCullDistances: отсечение клеток по расстоянию выключено"));
		}
	}

	CellsMeshHierarchical->SetCullDistances(CullStart, CullEnd);
	CellsMeshFlat->SetCullDistances(CullStart, CullEnd);

	// Компоненты рендера по возрасту (см. AgeMaterials) - те же дистанции,
	// иначе переключение в режим AgeMaterials молча теряло бы уже
	// подобранное отсечение по расстоянию.
	for (UInstancedStaticMeshComponent* AgeComponent : AgeMeshComponents)
	{
		if (AgeComponent)
		{
			AgeComponent->SetCullDistances(CullStart, CullEnd);
		}
	}

	if (SelectionMeshComponent)
	{
		SelectionMeshComponent->SetCullDistances(CullStart, CullEnd);
	}

	// Диагностика для отладки отсечения - пока порог включён (CullEnd > 0),
	// раз в секунду (не на каждый вызов - при высоком Speed это был бы спам)
	// печатаем всё, что нужно, чтобы понять, ПОЧЕМУ клетки не отсекаются:
	// позицию камеры, центр/радиус текущей сетки, реальное расстояние между
	// ними и то, что фактически осело на компоненте после SetCullDistances()
	// (не просто то, что мы передали - вдруг движок не принял значение).
	if (CullEnd > 0)
	{
		static double LastCullDebugLogSeconds = 0.0;
		const double NowSeconds = FPlatformTime::Seconds();
		if (NowSeconds - LastCullDebugLogSeconds >= 1.0)
		{
			LastCullDebugLogSeconds = NowSeconds;

			FVector CameraLocation = FVector::ZeroVector;
			const bool bHaveCamera = (GamePC != nullptr && GamePC->PlayerCameraManager != nullptr);
			if (bHaveCamera)
			{
				CameraLocation = GamePC->PlayerCameraManager->GetCameraLocation();
			}

			FVector GridCenter = FVector::ZeroVector;
			float GridRadius = 0.0f;
			const bool bHaveBounds = ComputeAliveCellsBounds(GridCenter, GridRadius);

			const float DistanceToCenter = (bHaveCamera && bHaveBounds)
				? FVector::Dist(CameraLocation, GridCenter)
				: -1.0f;

			int32 ActualStart = 0;
			int32 ActualEnd = 0;
			CellsMeshHierarchical->GetCullDistances(ActualStart, ActualEnd);

			UE_LOG(LogTemp, Log, TEXT("ApplyCellCullDistances: [cull debug] камера=%s (есть=%d), центр сетки=%s радиус=%.1f (есть=%d), расстояние камера-центр=%.1f, задано Start/End=%d/%d, реально на CellsMeshHierarchical Start/End=%d/%d"),
				*CameraLocation.ToString(), bHaveCamera ? 1 : 0,
				*GridCenter.ToString(), GridRadius, bHaveBounds ? 1 : 0,
				DistanceToCenter,
				CullStart, CullEnd,
				ActualStart, ActualEnd);
		}
	}
}

void AAutomataOrchestrator::RebuildAgeMeshComponents()
{
	// После реинстансинга Live Coding'ом (правка reflection-полей класса,
	// например самого AgeMaterials) AgeMeshComponents (UPROPERTY) переживает
	// пересборку класса, а AgeRenderers (обычный член) - нет, обнуляется
	// вместе с новым экземпляром. Пересинхронизируем рендереры вокруг уже
	// существующих компонентов, не трогая сами компоненты, прежде чем решать,
	// нужно ли расти/сокращаться дальше.
	if (AgeRenderers.Num() != AgeMeshComponents.Num())
	{
		AgeRenderers.Reset();
		for (UInstancedStaticMeshComponent* ExistingComponent : AgeMeshComponents)
		{
			AgeRenderers.Add(MakeUnique<FInstancedMeshCellGridRenderer>(ExistingComponent));
		}
	}

	const bool bHierarchical = (CellMeshComponentType == ECellMeshComponentType::HierarchicalInstanced);
	const bool bExistingAreHierarchical = (AgeMeshComponents.Num() > 0)
		&& (Cast<UHierarchicalInstancedStaticMeshComponent>(AgeMeshComponents[0]) != nullptr);

	if (AgeMeshComponents.Num() > 0 && bExistingAreHierarchical != bHierarchical)
	{
		// CellMeshComponentType поменяли - пересоздаём весь пул под новый
		// класс. В отличие от CellsMeshFlat/CellsMeshHierarchical (CreateDefaultSubobject,
		// не пересоздаются никогда - см. их doc-comment), эти компоненты
		// созданы рантаймом через NewObject(), так что уничтожить и
		// пересоздать их безопасно - это не тот сценарий с Live Coding,
		// который бьёт по default subobject'ам.
		for (UInstancedStaticMeshComponent* OldComponent : AgeMeshComponents)
		{
			if (OldComponent)
			{
				OldComponent->DestroyComponent();
			}
		}
		AgeMeshComponents.Reset();
		AgeRenderers.Reset();
	}

	const int32 DesiredNum = AgeMaterials.Num();

	while (AgeMeshComponents.Num() > DesiredNum)
	{
		if (UInstancedStaticMeshComponent* ExcessComponent = AgeMeshComponents.Pop())
		{
			ExcessComponent->DestroyComponent();
		}
		AgeRenderers.Pop();
	}

	while (AgeMeshComponents.Num() < DesiredNum)
	{
		UInstancedStaticMeshComponent* NewComponent = bHierarchical
			? static_cast<UInstancedStaticMeshComponent*>(NewObject<UHierarchicalInstancedStaticMeshComponent>(this))
			: NewObject<UInstancedStaticMeshComponent>(this);

		NewComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		NewComponent->SetupAttachment(CellsMeshHierarchical);
		NewComponent->RegisterComponent();

		AgeMeshComponents.Add(NewComponent);
		AgeRenderers.Add(MakeUnique<FInstancedMeshCellGridRenderer>(NewComponent));
	}
}

void AAutomataOrchestrator::EnsureSelectionMeshComponent()
{
	if (SelectionMeshComponent && !SelectionRenderer)
	{
		// Пережил реинстансинг Live Coding (UPROPERTY), а SelectionRenderer
		// (обычный член) - нет, см. doc-comment AgeMeshComponents про тот же
		// сценарий.
		SelectionRenderer = MakeUnique<FInstancedMeshCellGridRenderer>(SelectionMeshComponent);
	}

	if (SelectionMeshComponent)
	{
		return;
	}

	// Всегда обычный ISM, независимо от CellMeshComponentType - выделение
	// всегда маленькое подмножество, LOD-дерево кластеров HISM тут не даёт
	// выигрыша (см. doc-comment SelectionMeshComponent в заголовке).
	SelectionMeshComponent = NewObject<UInstancedStaticMeshComponent>(this);
	SelectionMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SelectionMeshComponent->SetupAttachment(CellsMeshHierarchical);
	SelectionMeshComponent->RegisterComponent();

	SelectionRenderer = MakeUnique<FInstancedMeshCellGridRenderer>(SelectionMeshComponent);
}

void AAutomataOrchestrator::RenderSelectionOverlay()
{
	EnsureSelectionMeshComponent();

	if (!Grid || SelectedCells.Num() == 0 || !SelectionMaterial)
	{
		SelectionMeshComponent->ClearInstances();
		return;
	}

	// Отфильтровываем до реально живых - на случай, если SelectedCells
	// вызвали до какого-то не прошедшего через инвалидацию изменения Grid
	// (сегодня такого пути нет, но проверка дешёвая, а рассинхрон иначе тихий).
	TArray<FIntVector> AliveSelected;
	AliveSelected.Reserve(SelectedCells.Num());
	for (const FIntVector& Cell : SelectedCells)
	{
		if (Grid->IsAlive(Cell))
		{
			AliveSelected.Add(Cell);
		}
	}

	if (AliveSelected.Num() == 0)
	{
		SelectionMeshComponent->ClearInstances();
		return;
	}

	SelectionRenderer->SetMesh(CellMesh);
	SelectionRenderer->SetMaterial(SelectionMaterial);

	FFilteredCellGridView SelectionView(*Grid, MoveTemp(AliveSelected));
	// Всегда одним снимком - выделение всегда маленькое, чанкинг не нужен
	// даже во время непрерывного Play.
	SelectionRenderer->Render(SelectionView);
}

void AAutomataOrchestrator::SelectCellsInScreenRect(const FMatrix& ViewProjectionMatrix, const FVector2D& ViewportSize, const FVector2D& RectMin, const FVector2D& RectMax)
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellsInScreenRect: сетка не инициализирована"));
		return;
	}

	SelectedCells = CellSelection::SelectCellsInScreenRect(*Grid, ViewProjectionMatrix, ViewportSize, RectMin, RectMax);
	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("SelectCellsInScreenRect: выделено %d клеток"), SelectedCells.Num());
}

void AAutomataOrchestrator::StartFromSelection()
{
	if (SelectedCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: нет выделенных клеток - сначала выделите что-нибудь мышкой в режиме выделения (C)"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: сетка не инициализирована"));
		return;
	}

	if (AgeMaterials.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: AgeMaterials пуст - назначьте хотя бы один материал в Details panel"));
		return;
	}

	// Мировые координаты НЕ переносятся к началу координат - клетки остаются
	// там же, где их выделили (правила автомата трансляционно инвариантны, а
	// камера и так уже смотрит именно туда - см. doc-comment в заголовке).
	TUniquePtr<FCellGrid> NewGrid = CreateGrid();
	int32 SpawnedCount = 0;
	for (const FIntVector& Cell : SelectedCells)
	{
		if (Grid->IsAlive(Cell))
		{
			NewGrid->SetAlive(Cell, true);
			NewGrid->SetAge(Cell, 0); // свежий старт, как только что рождённая клетка
			++SpawnedCount;
		}
	}

	Grid = MoveTemp(NewGrid);
	SelectedCells.Reset();
	StepsSinceLastRender = 0;

	if (GamePC)
	{
		GamePC->SetSelectionModeActive(false);
	}

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("StartFromSelection: новое состояние из %d клеток"), SpawnedCount);
}

bool AAutomataOrchestrator::ComputeAliveCellsBounds(FVector& OutCenter, float& OutRadius) const
{
	if (!Grid || Grid->Num() == 0)
	{
		return false;
	}

	TArray<FIntVector> AliveCells;
	Grid->GetAliveCells(AliveCells);

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

	const FVector WorldMin = Grid->GridToWorld(MinCell);
	const FVector WorldMax = Grid->GridToWorld(MaxCell);

	OutCenter = (WorldMin + WorldMax) * 0.5;
	// Половина диагонали AABB (радиус описанной сферы) плюс запас на
	// полклетки - GridToWorld() даёт координаты центра клетки, а не её края.
	OutRadius = (WorldMax - WorldMin).Size() * 0.5f + Grid->GetCellSize() * 0.5f;

	return true;
}

TUniquePtr<FCellGrid> AAutomataOrchestrator::CreateGrid() const
{
	return MakeUnique<FDenseCellGrid>(CellSize, ChunkSize);
}

TUniquePtr<FCellularAutomatonComputeStrategy> AAutomataOrchestrator::CreateComputeStrategy() const
{
	switch (ComputeMethod)
	{
	case EComputeMethod::Gpu:
		return MakeUnique<FGpuComputeStrategy>(GpuVolumeCellLimit);
	case EComputeMethod::Cpu:
	default:
		return MakeUnique<FCpuComputeStrategy>();
	}
}

void AAutomataOrchestrator::GenerateRandom()
{
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandom: фоновый шаг StepAsync() ещё считается - подождите его завершения"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandom: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandom: активный CellsMesh-компонент отсутствует"));
		return;
	}

	if (AgeMaterials.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandom: AgeMaterials пуст - назначьте хотя бы один материал в Details panel"));
		return;
	}

	// GenerateRandom() всегда генерирует новое состояние с нуля и подхватывает
	// актуальный CellSize, если его поменяли в Details panel
	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	// Новая сетка делает старое выделение бессмысленным (координаты уже не
	// про эту сетку) - см. doc-comment SelectedCells в заголовке.
	SelectedCells.Reset();

	// Инициализируем ГСЧ фиксированным сидом для воспроизводимости
	FRandomStream RandomStream(Seed);

	const float RadiusInCells = static_cast<float>(SpawnRadius);

	const double GenerationStartSeconds = FPlatformTime::Seconds();

	for (int32 i = 0; i < Amount; ++i)
	{
		// Reject-sampling: точка в кубе [-Radius, +Radius], отбрасываем если вне сферы
		FVector SamplePoint;
		do
		{
			SamplePoint = FVector(
				RandomStream.FRandRange(-RadiusInCells, RadiusInCells),
				RandomStream.FRandRange(-RadiusInCells, RadiusInCells),
				RandomStream.FRandRange(-RadiusInCells, RadiusInCells));
		}
		while (SamplePoint.SizeSquared() > FMath::Square(RadiusInCells));

		// Округляем до ближайшей целой клетки сетки
		const FIntVector GridCell(
			FMath::RoundToInt(SamplePoint.X),
			FMath::RoundToInt(SamplePoint.Y),
			FMath::RoundToInt(SamplePoint.Z));

		Grid->SetAlive(GridCell, true);
	}

	const double GenerationSeconds = FPlatformTime::Seconds() - GenerationStartSeconds;

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("GenerateRandom: заспавнено %d клеток в радиусе %d (генерация: %.2f мс)"),
		Grid->Num(), SpawnRadius, GenerationSeconds * 1000.0);
}

void AAutomataOrchestrator::Next()
{
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: фоновый шаг StepAsync() ещё считается - подождите его завершения"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: сетка не инициализирована - сначала вызовите GenerateRandom"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: активный CellsMesh-компонент отсутствует"));
		return;
	}

	if (AgeMaterials.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: AgeMaterials пуст - назначьте хотя бы один материал в Details panel"));
		return;
	}

	// Строим правило заново на каждый вызов, чтобы правки BirthCounts/
	// SurvivalCounts/Neighborhood в Details panel подхватывались немедленно
	// (аналогично тому, как GenerateRandom() каждый раз пересоздаёт Grid,
	// а не кэширует его)
	const FCellularAutomatonRule AutomatonRule(BirthCounts, SurvivalCounts, Neighborhood);
	const TUniquePtr<FCellularAutomatonComputeStrategy> ComputeStrategy = CreateComputeStrategy();

	TUniquePtr<FCellGrid> NextGrid = CreateGrid();

	const double StepStartSeconds = FPlatformTime::Seconds();
	ComputeStrategy->Step(*Grid, *NextGrid, AutomatonRule);
	CellAging::ComputeAges(Grid.Get(), *NextGrid);
	const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;

	Grid = MoveTemp(NextGrid);
	SelectedCells.Reset();

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("Next: живых клеток %d после шага (шаг: %.2f мс)"),
		Grid->Num(), StepSeconds * 1000.0);
}

void AAutomataOrchestrator::StepAsync()
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: сетка не инициализирована - сначала вызовите GenerateRandom"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: активный CellsMesh-компонент отсутствует"));
		return;
	}

	if (AgeMaterials.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: AgeMaterials пуст - назначьте хотя бы один материал в Details panel"));
		return;
	}

	// Правило, стратегия расчёта и буфер следующего поколения строим здесь,
	// на game thread - все три читают UPROPERTY (BirthCounts/SurvivalCounts/
	// Neighborhood/ComputeMethod/GpuVolumeCellLimit/CellSize/ChunkSize),
	// которые могут одновременно редактироваться в Details panel. После этой
	// точки фоновый поток их больше не касается - только *Grid (на чтение) и
	// NextGridBuffer (на запись, свежесозданный, ни с кем не общий).
	FCellularAutomatonRule AutomatonRule(BirthCounts, SurvivalCounts, Neighborhood);
	TUniquePtr<FCellularAutomatonComputeStrategy> ComputeStrategy = CreateComputeStrategy();
	TUniquePtr<FCellGrid> NextGridBuffer = CreateGrid();

	bStepInProgress = true;

	FCellGrid* CurrentGridPtr = Grid.Get();
	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	// CurrentGridPtr - сырой указатель на *Grid, без защиты времени жизни -
	// PendingStepFuture даёт EndPlay() дождаться завершения этого фонового
	// шага перед тем, как актор (а с ним и Grid) начнёт разрушаться.
	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[AutomatonRule = MoveTemp(AutomatonRule), ComputeStrategy = MoveTemp(ComputeStrategy),
		 NextGridBuffer = MoveTemp(NextGridBuffer), CurrentGridPtr, WeakThis]() mutable
		{
			const double StepStartSeconds = FPlatformTime::Seconds();
			ComputeStrategy->Step(*CurrentGridPtr, *NextGridBuffer, AutomatonRule);
			CellAging::ComputeAges(CurrentGridPtr, *NextGridBuffer);
			const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;

			// Grid/рендер трогаем только на game thread - AsyncTask сюда и
			// маршрутизирует. WeakThis - на случай, если актор уничтожили
			// (например, level unload) пока фоновый Step() ещё считался.
			AsyncTask(ENamedThreads::GameThread, [WeakThis, NextGridBuffer = MoveTemp(NextGridBuffer), StepSeconds]() mutable
			{
				if (AAutomataOrchestrator* StrongThis = WeakThis.Get())
				{
					StrongThis->ApplyStepResult(MoveTemp(NextGridBuffer), StepSeconds);
				}
			});
		});
}

TArray<TArray<FIntVector>> AAutomataOrchestrator::BuildAgeBuckets() const
{
	const int32 NumBuckets = AgeMaterials.Num();

	// Бакетируем живые клетки по возрасту: MaterialIndex = N-1-min(Age, N-1) -
	// последний материал массива достаётся самым молодым (Age=0) клеткам,
	// первый - клеткам, доживших до (N-1) эпох и старше (см. doc-comment
	// AgeMaterials).
	TArray<FIntVector> AliveCells;
	Grid->GetAliveCells(AliveCells);

	TArray<TArray<FIntVector>> Buckets;
	Buckets.SetNum(NumBuckets);
	for (const FIntVector& Cell : AliveCells)
	{
		const uint8 Age = Grid->GetAge(Cell);
		const int32 MaterialIndex = NumBuckets - 1 - FMath::Min(static_cast<int32>(Age), NumBuckets - 1);
		Buckets[MaterialIndex].Add(Cell);
	}

	return Buckets;
}

void AAutomataOrchestrator::RenderGridImmediate()
{
	if (AgeMaterials.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("RenderGridImmediate: AgeMaterials пуст - рендер пропущен"));
		return;
	}

	RebuildAgeMeshComponents();
	ApplyCellCullDistances();

	// Без этого старый снимок на базовом компоненте (например, оставшийся
	// от версии до появления AgeMaterials/из старого сохранённого уровня)
	// остаётся виден одновременно с новыми возрастными компонентами.
	if (UInstancedStaticMeshComponent* BaseComponent = GetActiveCellsMeshComponent())
	{
		BaseComponent->ClearInstances();
	}

	const int32 NumBuckets = AgeMaterials.Num();
	TArray<TArray<FIntVector>> Buckets = BuildAgeBuckets();

	for (int32 MaterialIndex = 0; MaterialIndex < NumBuckets; ++MaterialIndex)
	{
		FInstancedMeshCellGridRenderer* BucketRenderer = AgeRenderers[MaterialIndex].Get();
		BucketRenderer->SetMesh(CellMesh);
		BucketRenderer->SetMaterial(AgeMaterials[MaterialIndex]);

		FFilteredCellGridView FilteredView(*Grid, MoveTemp(Buckets[MaterialIndex]));
		// Всегда одним снимком (не BeginRender()/чанкинг) - Next()/GenerateRandom()
		// рендерят немедленно и целиком, независимо от bEnableChunkedRender
		// (см. doc-comment RenderGridImmediate() в заголовке).
		BucketRenderer->Render(FilteredView);
	}

	// Не-op, если SelectedCells пуст (свежая сетка/шаг уже его сбросили) -
	// сам чистит SelectionMeshComponent в этом случае.
	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("RenderGridImmediate: живых клеток %d отрисовано по %d материалам (одним снимком)"),
		Grid->Num(), NumBuckets);
}

void AAutomataOrchestrator::RenderCurrentGrid()
{
	if (AgeMaterials.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("RenderCurrentGrid: AgeMaterials пуст - рендер пропущен"));
		return;
	}

	RebuildAgeMeshComponents();
	ApplyCellCullDistances();

	// Без этого старый снимок на базовом компоненте (например, от Next()/
	// GenerateRandom(), которые сами тоже рендерят через RenderGridImmediate() -
	// на самом деле этого не произойдёт, но проверка дешёвая - или от старого
	// сохранённого уровня) остаётся виден одновременно с новыми возрастными
	// компонентами.
	if (UInstancedStaticMeshComponent* BaseComponent = GetActiveCellsMeshComponent())
	{
		BaseComponent->ClearInstances();
	}

	const int32 NumBuckets = AgeMaterials.Num();
	TArray<TArray<FIntVector>> Buckets = BuildAgeBuckets();

	const FVector CameraLocation = (GamePC && GamePC->PlayerCameraManager)
		? GamePC->PlayerCameraManager->GetCameraLocation()
		: FVector::ZeroVector;

	for (int32 MaterialIndex = 0; MaterialIndex < NumBuckets; ++MaterialIndex)
	{
		FInstancedMeshCellGridRenderer* BucketRenderer = AgeRenderers[MaterialIndex].Get();
		BucketRenderer->SetMesh(CellMesh);
		BucketRenderer->SetMaterial(AgeMaterials[MaterialIndex]);

		FFilteredCellGridView FilteredView(*Grid, MoveTemp(Buckets[MaterialIndex]));

		if (bEnableChunkedRender)
		{
			BucketRenderer->BeginRender(FilteredView, ChunkedRenderOrder, CameraLocation);
		}
		else
		{
			BucketRenderer->Render(FilteredView);
		}
	}

	// Подсветка выделения - всегда одним снимком (не чанкуется, выделение
	// всегда маленькое подмножество), не-op, если SelectedCells пуст.
	RenderSelectionOverlay();

	if (bEnableChunkedRender)
	{
		bChunkedRenderInProgress = true;
		ChunkedRenderStartSeconds = FPlatformTime::Seconds();
		ChunkedRenderFrameCount = 0;

		UE_LOG(LogTemp, Log, TEXT("RenderCurrentGrid: живых клеток %d по %d материалам - рендер разлит по кадрам"),
			Grid->Num(), NumBuckets);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("RenderCurrentGrid: живых клеток %d отрисовано по %d материалам"),
			Grid->Num(), NumBuckets);
	}
}

void AAutomataOrchestrator::ApplyStepResult(TUniquePtr<FCellGrid> NewGrid, double StepSeconds)
{
	Grid = MoveTemp(NewGrid);
	// Новое поколение делает старое выделение бессмысленным - сбрасываем
	// сразу, независимо от того, дойдёт ли дело до фактического рендера ниже
	// (см. doc-comment SelectedCells в заголовке).
	SelectedCells.Reset();

	// Сужено до конца фонового чтения Grid - дальше (рендер, возможный
	// чанковый "разлив") фонового потока уже не касается, так что следующий
	// StepAsync() может стартовать независимо от того, что происходит с
	// рендером ниже.
	bStepInProgress = false;

	++StepsSinceLastRender;
	if (StepsSinceLastRender < StepsPerRender)
	{
		UE_LOG(LogTemp, Log, TEXT("StepAsync: живых клеток %d после шага (шаг: %.2f мс [фоновый поток]) - рендер пропущен (%d/%d)"),
			Grid->Num(), StepSeconds * 1000.0, StepsSinceLastRender, StepsPerRender);
		return;
	}
	StepsSinceLastRender = 0;

	// Если предыдущий чанковый "разлив" ещё не дорисовался - по умолчанию не
	// ждём его окончания, а прерываем немедленно: RenderCurrentGrid() ниже
	// вызывает BeginRender(), который сам делает ClearInstances() и
	// перестраивает PendingTransforms с нуля по уже подставленному Grid, так
	// что недорисованные инстансы прошлого поколения просто никогда не
	// попадут на экран. Пока включён bWaitForChunkedRenderToFinish, эта ветка
	// физически не должна срабатывать - Tick() уже не запускает StepAsync(),
	// пока bChunkedRenderInProgress истинен (см. bBlockedByChunkedRender в
	// Tick()), так что сюда мы попадаем только с уже завершённым разливом.
	if (bChunkedRenderInProgress)
	{
		UE_LOG(LogTemp, Log, TEXT("StepAsync: живых клеток %d после шага (шаг: %.2f мс [фоновый поток]) - предыдущий разлив прерван, рендерим новое состояние"),
			Grid->Num(), StepSeconds * 1000.0);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("StepAsync: шаг занял %.2f мс [фоновый поток]"), StepSeconds * 1000.0);
	}
	RenderCurrentGrid();
}

void AAutomataOrchestrator::SetChunkedRenderEnabled(bool bEnabled)
{
	bEnableChunkedRender = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("SetChunkedRenderEnabled: рендер по кадрам %s"), bEnabled ? TEXT("включён") : TEXT("выключен"));
}

void AAutomataOrchestrator::CycleChunkedRenderOrder()
{
	constexpr uint8 NumOrders = (uint8)EChunkedRenderOrder::FromCenterOutward + 1;
	ChunkedRenderOrder = (EChunkedRenderOrder)(((uint8)ChunkedRenderOrder + 1) % NumOrders);
	UE_LOG(LogTemp, Log, TEXT("CycleChunkedRenderOrder: порядок реавила -> %s"), *UEnum::GetValueAsString(ChunkedRenderOrder));
}

void AAutomataOrchestrator::SetWaitForChunkedRenderToFinish(bool bWait)
{
	bWaitForChunkedRenderToFinish = bWait;
	UE_LOG(LogTemp, Log, TEXT("SetWaitForChunkedRenderToFinish: режим ожидания разлива %s"), bWait ? TEXT("включён") : TEXT("выключен"));
}

void AAutomataOrchestrator::SetCellCullingEnabled(bool bEnabled)
{
	bEnableCellCulling = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("SetCellCullingEnabled: отсечение клеток по расстоянию %s"), bEnabled ? TEXT("включено") : TEXT("выключено"));

	// Применяем немедленно, не дожидаясь следующего рендера (см. doc-comment
	// ApplyCellCullDistances()) - иначе переключение хоткеем B, пока новое
	// поколение не рендерится, визуально ничего не меняло до следующего шага.
	ApplyCellCullDistances();
}

void AAutomataOrchestrator::AdvanceChunkedRender()
{
	++ChunkedRenderFrameCount;

	const int32 NumBuckets = AgeRenderers.Num();
	// Бюджет ChunkedRenderCellsPerFrame делится между бакетами, а не
	// применяется к каждому целиком - иначе цена кадра умножилась бы на
	// число материалов незаметно для пользователя (см. doc-comment
	// AdvanceChunkedRender() в заголовке).
	const int32 PerBucketBudget = FMath::Max(1, ChunkedRenderCellsPerFrame / FMath::Max(1, NumBuckets));

	bool bMoreRemaining = false;
	for (const TUniquePtr<FInstancedMeshCellGridRenderer>& BucketRenderer : AgeRenderers)
	{
		bMoreRemaining |= BucketRenderer->AdvanceRenderChunk(PerBucketBudget);
	}

	if (bMoreRemaining)
	{
		return;
	}

	bChunkedRenderInProgress = false;

	const double TotalSeconds = FPlatformTime::Seconds() - ChunkedRenderStartSeconds;
	UE_LOG(LogTemp, Log, TEXT("AdvanceChunkedRender: рендер разлитый по кадрам завершён (%d материалов) - живых клеток %d за %d кадр(ов)/%.2f мс"),
		NumBuckets, Grid->Num(), ChunkedRenderFrameCount, TotalSeconds * 1000.0);
}

void AAutomataOrchestrator::FinishChunkedRenderImmediately()
{
	if (!bChunkedRenderInProgress)
	{
		return;
	}

	for (const TUniquePtr<FInstancedMeshCellGridRenderer>& BucketRenderer : AgeRenderers)
	{
		while (BucketRenderer->AdvanceRenderChunk(TNumericLimits<int32>::Max()))
		{
		}
	}

	bChunkedRenderInProgress = false;

	UE_LOG(LogTemp, Log, TEXT("FinishChunkedRenderImmediately: чанковый рендер довершён одним разом (остановлен через Stop, %d материалов) - живых клеток %d"),
		AgeRenderers.Num(), Grid->Num());
}

void AAutomataOrchestrator::StartFastStep()
{
	if (bSimulationRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFastStep: симуляция уже запущена через Play (P) - остановите её сначала"));
		return;
	}

	bFastStepActive = true;
	TimeSinceLastStep = 0.0f;
	StepsSinceLastRender = 0;
	SetActorTickEnabled(true);

	UE_LOG(LogTemp, Log, TEXT("StartFastStep: автошаг (Shift+F) включён"));
}

void AAutomataOrchestrator::StopFastStep()
{
	bFastStepActive = false;

	if (!bSimulationRunning && !bChunkedRenderInProgress)
	{
		SetActorTickEnabled(false);
	}

	UE_LOG(LogTemp, Log, TEXT("StopFastStep: автошаг выключен"));
}

void AAutomataOrchestrator::Start()
{
	if (IsFastStepActive())
	{
		UE_LOG(LogTemp, Warning, TEXT("Start: активен автошаг по F - остановите его (повторное F / отпустить Shift+F) перед запуском Play"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("Start game"));
	Resume();

	if (!Grid)
	{
		GenerateRandom();
	}

	TimeSinceLastStep = 0.0f;
	StepsSinceLastRender = 0;
	bSimulationRunning = true;
	SetActorTickEnabled(true);
}

void AAutomataOrchestrator::Pause()
{
	if (!GamePC)
	{
		UE_LOG(LogTemp, Warning, TEXT("Pause: GamePC не назначен - PlayerController не найден"));
		return;
	}
	GamePC->SetCameraControlEnabled(false);
}
void AAutomataOrchestrator::Resume()
{
	if (!GamePC)
	{
		UE_LOG(LogTemp, Warning, TEXT("Resume: GamePC не назначен - PlayerController не найден"));
		return;
	}
	GamePC->SetCameraControlEnabled(true);
}

void AAutomataOrchestrator::Stop()
{
	bSimulationRunning = false;

	// Если чанковый рендер ещё не доехал по кадрам - досыпаем его одним
	// разом сейчас, а не оставляем недорисованным: SetActorTickEnabled(false)
	// ниже иначе останавливает Tick() (а с ним и AdvanceChunkedRender())
	// прямо здесь, замораживая "разлив" навсегда до следующего Start()/шага.
	FinishChunkedRenderImmediately();

	SetActorTickEnabled(false);
}

void AAutomataOrchestrator::Clear()
{
	
}