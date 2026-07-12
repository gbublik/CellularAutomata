// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "Automata/Rendering/FilteredCellGridView.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Kismet/GameplayStatics.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/CellAging.h"
#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"
#include "Automata/Simulation/ComputeStrategy/GpuComputeStrategy.h"
#include "Automata/Selection/CellSelection.h"
#include "Automata/Meshing/CellMeshBuilder.h"
#include "Automata/Meshing/ChunkGridView.h"
#include "Automata/Persistence/AutomatonStateSerializer.h"
#include "ProceduralMeshComponent.h"
#include "Async/Async.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "UnrealClient.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"

// Сглаженный FPS движка - определён в UnrealEngine.cpp, без публичного
// заголовка, объявляется локально там, где используется (тот же паттерн,
// что и в самом движке, см. напр. EngineAnalyticsSessionSummary.cpp) - см.
// FHudStats::CurrentFPS в Tick().
extern ENGINE_API float GAverageFPS;

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

	// Раньше вызывалось из PostActorCreated(), который срабатывает при любом
	// создании актора - в т.ч. просто при расстановке в редакторе вне PIE, не
	// только в игре. HUD-виджет никогда не должен всплывать вне реальной
	// игровой сессии - BeginPlay() гарантированно только PIE/игра.
	InitializeHUD();
}

// Called every frame
void AAutomataOrchestrator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// HUD-сводка (см. FHudStats) обновляется каждый тик, ДО веток
	// bFastStepActive/!bSimulationRunning ниже (у обеих есть ранний return) -
	// HUD должен показывать FPS/занятость даже когда симуляция на паузе, не
	// только пока Play/автошаг активны.
	LastHudStats.bIsComputing = bStepInProgress;
	LastHudStats.bIsRendering = bChunkedRenderInProgress;
	LastHudStats.CurrentFPS = GAverageFPS;
	LastHudStats.GenerationCount = GenerationCount;
	LastHudStats.EstimatedGpuComputeUploadMB = LastGpuComputeUploadBytes / (1024.0 * 1024.0);
	UpdateGenerationsPerSecond();

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

void AAutomataOrchestrator::UpdateGenerationsPerSecond()
{
	const double Now = FPlatformTime::Seconds();
	const double Elapsed = Now - LastGenerationCountSampleSeconds;

	// Раз в секунду, не каждый кадр - на коротких интервалах (доли секунды)
	// частота "поколений в секунду" скачет шумно (особенно при малых Speed),
	// секундное окно даёт стабильное на глаз число, тот же дух, что и
	// остальные "раз в N" пересчёты в проекте (см. AdvanceChunkedRender()'s
	// итоговый лог только по завершении разлива, не каждый кадр).
	if (Elapsed < 1.0)
	{
		return;
	}

	LastHudStats.GenerationsPerSecond = float((GenerationCount - LastGenerationCountSample) / Elapsed);
	LastGenerationCountSample = GenerationCount;
	LastGenerationCountSampleSeconds = Now;
}

void AAutomataOrchestrator::ResetGenerationCounter()
{
	GenerationCount = 0;
	LastGenerationCountSample = 0;
	LastGenerationCountSampleSeconds = FPlatformTime::Seconds();
	LastHudStats.GenerationCount = 0;
	LastHudStats.GenerationsPerSecond = 0.0f;
	LastGpuComputeUploadBytes = 0;
	LastHudStats.EstimatedGpuComputeUploadMB = 0.0;
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

	if (SelectedCells.Num() > 0 && !SelectionMaterial)
	{
		// Иначе подсветка молча не рисуется, и выглядит это как "выделение
		// не работает" - уже кусало при настройке.
		UE_LOG(LogTemp, Warning, TEXT("RenderSelectionOverlay: SelectionMaterial не назначен - подсветка выделения не будет видна, назначьте материал в Details panel"));
	}

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
	// Чуть крупнее обычного кубика - иначе поверхности совпадают и мерцают
	// (z-fighting), см. doc-comment SelectionScaleMultiplier.
	SelectionRenderer->SetScaleMultiplier(SelectionScaleMultiplier);

	FFilteredCellGridView SelectionView(*Grid, MoveTemp(AliveSelected));
	// Всегда одним снимком - выделение всегда маленькое, чанкинг не нужен
	// даже во время непрерывного Play.
	SelectionRenderer->Render(SelectionView);
}

void AAutomataOrchestrator::SelectCellsInScreenRect(const FMatrix& ViewProjectionMatrix, const FVector2D& ViewportSize, const FVector2D& RectMin, const FVector2D& RectMax, ESelectionCombineMode CombineMode)
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellsInScreenRect: сетка не инициализирована"));
		return;
	}

	TArray<FIntVector> RectCells = CellSelection::SelectCellsInScreenRect(*Grid, ViewProjectionMatrix, ViewportSize, RectMin, RectMax);
	CombineWithSelection(MoveTemp(RectCells), CombineMode);

	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("SelectCellsInScreenRect: выделено %d клеток (режим: %s)"),
		SelectedCells.Num(), *UEnum::GetValueAsString(CombineMode));
}

void AAutomataOrchestrator::SelectCellUnderCursor(const FVector& RayOrigin, const FVector& RayDirection, ESelectionCombineMode CombineMode)
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellUnderCursor: сетка не инициализирована"));
		return;
	}

	// Лимит обхода DDA: до дальнего края описанной сферы живых клеток -
	// дальше живых клеток гарантированно нет, шагать бессмысленно.
	FVector BoundsCenter = FVector::ZeroVector;
	float BoundsRadius = 0.0f;
	if (!ComputeAliveCellsBounds(BoundsCenter, BoundsRadius))
	{
		UE_LOG(LogTemp, Log, TEXT("SelectCellUnderCursor: живых клеток нет - выделять нечего"));
		return;
	}
	const double MaxDistance = FVector::Distance(RayOrigin, BoundsCenter) + BoundsRadius + Grid->GetCellSize();

	TArray<FIntVector> PickedCells;
	FIntVector PickedCell;
	if (CellSelection::PickCellAlongRay(*Grid, RayOrigin, RayDirection, MaxDistance, PickedCell))
	{
		PickedCells.Add(PickedCell);
	}

	// Пустой PickedCells (клик в пустоту) - тоже валидный ввод: Replace
	// очистит выделение (стандартное "кликнул мимо - снял выделение"),
	// Add/Subtract ничего не изменят.
	CombineWithSelection(MoveTemp(PickedCells), CombineMode);

	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("SelectCellUnderCursor: выделено %d клеток (режим: %s)"),
		SelectedCells.Num(), *UEnum::GetValueAsString(CombineMode));
}

void AAutomataOrchestrator::CombineWithSelection(TArray<FIntVector>&& NewCells, ESelectionCombineMode CombineMode)
{
	switch (CombineMode)
	{
	case ESelectionCombineMode::Add:
	{
		// Объединение без дублей: TSet по уже выделенным даёт O(1) проверку
		// на каждую новую клетку - выделения могут быть миллионными,
		// квадратичный Contains по TArray здесь недопустим.
		TSet<FIntVector> ExistingCells(SelectedCells);
		for (const FIntVector& Cell : NewCells)
		{
			if (!ExistingCells.Contains(Cell))
			{
				SelectedCells.Add(Cell);
			}
		}
		break;
	}
	case ESelectionCombineMode::Subtract:
	{
		const TSet<FIntVector> CellsToRemove(NewCells);
		SelectedCells.RemoveAll([&CellsToRemove](const FIntVector& Cell)
		{
			return CellsToRemove.Contains(Cell);
		});
		break;
	}
	case ESelectionCombineMode::Replace:
	default:
		SelectedCells = MoveTemp(NewCells);
		break;
	}
}

void AAutomataOrchestrator::DeleteSelectedCells()
{
	// Мутируем Grid - фоновый шаг (Next()/StepAsync()) в этот момент его
	// читает, тот же guard, что у всех путей изменения сетки.
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("DeleteSelectedCells: фоновый шаг ещё считается - подождите его завершения"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("DeleteSelectedCells: сетка не инициализирована"));
		return;
	}

	if (SelectedCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("DeleteSelectedCells: нет выделенных клеток - сначала выделите что-нибудь мышкой в режиме выделения (Tab)"));
		return;
	}

	if (AgeMaterials.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("DeleteSelectedCells: AgeMaterials пуст - назначьте хотя бы один материал в Details panel"));
		return;
	}

	int32 KilledCount = 0;
	for (const FIntVector& Cell : SelectedCells)
	{
		if (Grid->IsAlive(Cell))
		{
			Grid->SetAlive(Cell, false);
			++KilledCount;
		}
	}

	SelectedCells.Reset();
	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("DeleteSelectedCells: удалено %d клеток, живых осталось %d"), KilledCount, Grid->Num());
}

void AAutomataOrchestrator::InvertSelection()
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("InvertSelection: сетка не инициализирована"));
		return;
	}

	// TSet по текущему выделению - O(1) проверка на каждую живую клетку,
	// та же причина, что и в Add/Subtract-ветках SelectCellsInScreenRect().
	const TSet<FIntVector> CurrentlySelected(SelectedCells);

	TArray<FIntVector> AliveCells;
	Grid->GetAliveCells(AliveCells);

	TArray<FIntVector> Inverted;
	Inverted.Reserve(FMath::Max(0, AliveCells.Num() - SelectedCells.Num()));
	for (const FIntVector& Cell : AliveCells)
	{
		if (!CurrentlySelected.Contains(Cell))
		{
			Inverted.Add(Cell);
		}
	}

	SelectedCells = MoveTemp(Inverted);
	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("InvertSelection: выделено %d клеток (из %d живых)"), SelectedCells.Num(), AliveCells.Num());
}

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

void AAutomataOrchestrator::EnsureGhostMeshComponent()
{
	if (GhostMeshComponent)
	{
		return;
	}

	GhostMeshComponent = NewObject<UProceduralMeshComponent>(this);
	GhostMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostMeshComponent->SetupAttachment(CellsMeshHierarchical);
	GhostMeshComponent->RegisterComponent();
}

void AAutomataOrchestrator::ClearGhostShape()
{
	if (GhostMeshComponent)
	{
		GhostMeshComponent->ClearAllMeshSections();
	}
	GhostShapeGenerationsSinceRefresh = 0;
}

void AAutomataOrchestrator::RefreshGhostShape()
{
	if (!bEnableGhostShape || !Grid)
	{
		ClearGhostShape();
		return;
	}

	if (!bEnableRenderCullVolume)
	{
		// Выключен сам куб (хоткей C) - детальный путь теперь и так рисует
		// ВЕСЬ грид целиком (см. RenderGridImmediate()), "снаружи" куба
		// больше не существует, значит грубому силуэту нечего добавлять -
		// оставлять его висеть означало бы прозрачный дубль поверх уже
		// полностью нарисованных клеток. Тот же принцип, что и ниже (нет
		// самого актора куба), просто другая причина отсутствия границ.
		ClearGhostShape();
		return;
	}

	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	if (!CullVolume)
	{
		// Без активного куба отсекать не от чего - отсекать "снаружи" значит
		// "снаружи ничего" (отсекать нечего), фича молча ничего не делает,
		// не подменяет поведение (тот же принцип, что у bEnableRenderCullVolume).
		ClearGhostShape();
		return;
	}

	TArray<FIntVector> OccupiedChunks;
	Grid->GetOccupiedChunkCoords(OccupiedChunks);
	const float ChunkWorldSize = Grid->GetChunkWorldSize();
	if (OccupiedChunks.Num() == 0 || ChunkWorldSize <= 0.0f)
	{
		// ChunkWorldSize <= 0 - грид не поддерживает чанкинг (см. doc-comment
		// FCellGrid::GetChunkWorldSize()) - фича молча ничего не делает.
		ClearGhostShape();
		return;
	}

	// Оставляем только чанки СНАРУЖИ куба - внутри уже рисует обычный
	// детальный путь (BuildAgeBuckets()). Чанки на границе (частично
	// внутри/снаружи) сознательно остаются "внутри" (не отбрасываются) -
	// минимальное дублирование на границе дешевле точной обрезки.
	const FBox CullBounds = CullVolume->GetWorldBounds();
	TArray<FIntVector> OutsideChunks;
	OutsideChunks.Reserve(OccupiedChunks.Num());
	for (const FIntVector& ChunkCoord : OccupiedChunks)
	{
		const FVector ChunkOrigin = FVector(ChunkCoord) * ChunkWorldSize;
		const FBox ChunkBounds(ChunkOrigin, ChunkOrigin + FVector(ChunkWorldSize));
		if (!CullBounds.Intersect(ChunkBounds))
		{
			OutsideChunks.Add(ChunkCoord);
		}
	}

	if (OutsideChunks.Num() == 0)
	{
		ClearGhostShape();
		return;
	}

	UMaterialInterface* MeshMaterial = GhostShapeMaterial;
	if (!MeshMaterial && AgeMaterials.Num() > 0)
	{
		MeshMaterial = AgeMaterials[0];
		UE_LOG(LogTemp, Log, TEXT("RefreshGhostShape: GhostShapeMaterial не назначен - использую AgeMaterials[0]"));
	}

	const double BuildStartSeconds = FPlatformTime::Seconds();
	FChunkGridView ChunkView(ChunkWorldSize, OutsideChunks);
	CellMeshBuilder::FCellMeshData MeshData = CellMeshBuilder::BuildFromCells(ChunkView, OutsideChunks);
	const double BuildSeconds = FPlatformTime::Seconds() - BuildStartSeconds;

	EnsureGhostMeshComponent();
	GhostMeshComponent->ClearAllMeshSections();
	GhostMeshComponent->CreateMeshSection_LinearColor(0, MeshData.Vertices, MeshData.Triangles, MeshData.Normals, MeshData.UVs,
		TArray<FLinearColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision=*/false);
	if (MeshMaterial)
	{
		GhostMeshComponent->SetMaterial(0, MeshMaterial);
	}

	UE_LOG(LogTemp, Log, TEXT("RefreshGhostShape: %d/%d чанков снаружи куба -> %d вершин / %d треугольников (%.2f мс)"),
		OutsideChunks.Num(), OccupiedChunks.Num(), MeshData.Vertices.Num(), MeshData.Triangles.Num() / 3, BuildSeconds * 1000.0);
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
		return;
	}

	UMaterialInterface* MeshMaterial = BakedMeshMaterial;
	if (!MeshMaterial && AgeMaterials.Num() > 0)
	{
		MeshMaterial = AgeMaterials[0];
		UE_LOG(LogTemp, Log, TEXT("BakeCellsToMesh: BakedMeshMaterial не назначен - использую AgeMaterials[0]"));
	}

	const double BakeStartSeconds = FPlatformTime::Seconds();
	CellMeshBuilder::FCellMeshData MeshData = CellMeshBuilder::BuildFromCells(*Grid, CellsToBake);
	const double BuildSeconds = FPlatformTime::Seconds() - BakeStartSeconds;

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
	for (UInstancedStaticMeshComponent* AgeComponent : AgeMeshComponents)
	{
		if (AgeComponent)
		{
			AgeComponent->ClearInstances();
		}
	}
	if (SelectionMeshComponent)
	{
		SelectionMeshComponent->ClearInstances();
	}
	if (UInstancedStaticMeshComponent* BaseComponent = GetActiveCellsMeshComponent())
	{
		BaseComponent->ClearInstances();
	}
	bChunkedRenderInProgress = false;
	SelectedCells.Reset();
	// InitialStateCells намеренно НЕ трогаем - R после осмотра снимка
	// вернёт извлечённый паттерн, если он был (см. ResetToInitialState()).
	Grid.Reset();

	UE_LOG(LogTemp, Log, TEXT("BakeCellsToMesh: %d клеток -> %d вершин / %d треугольников (геометрия: %.2f мс, секция: %.2f мс); сетка и инстансы выгружены, R начнёт новый прогон"),
		CellsToBake.Num(), MeshData.Vertices.Num(), MeshData.Triangles.Num() / 3, BuildSeconds * 1000.0, SectionSeconds * 1000.0);
}

void AAutomataOrchestrator::StartFromSelection()
{
	// Фоновый шаг (Next()/StepAsync()) в этот момент читает *Grid - замена
	// сетки у него под ногами разыменует освобождённую память. Тот же guard,
	// что и в Next()/GenerateRandom()/ResetToInitialState().
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: фоновый шаг ещё считается - подождите его завершения"));
		return;
	}

	if (SelectedCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: нет выделенных клеток - сначала выделите что-нибудь мышкой в режиме выделения (Tab)"));
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
	// Заодно строим InitialStateCells - точно тот же набор, что реально
	// попал в NewGrid (только реально живые из SelectedCells), а не сырой
	// SelectedCells, который в принципе мог содержать неактуальные записи -
	// это и есть "точка возврата" для последующего ResetToInitialState() (R).
	InitialStateCells.Reset();
	InitialStateCells.Reserve(SelectedCells.Num());
	for (const FIntVector& Cell : SelectedCells)
	{
		if (Grid->IsAlive(Cell))
		{
			NewGrid->SetAlive(Cell, true);
			NewGrid->SetAge(Cell, 0); // свежий старт, как только что рождённая клетка
			InitialStateCells.Add(Cell);
		}
	}

	Grid = MoveTemp(NewGrid);
	SelectedCells.Reset();
	StepsSinceLastRender = 0;
	ResetGenerationCounter();
	// Новый прогон убирает запечённый меш-снимок, если он был (см.
	// BakeCellsToMesh()) - как и в GenerateRandom()/ResetToInitialState().
	ClearBakedMesh();
	ClearGhostShape();

	if (GamePC)
	{
		GamePC->SetSelectionModeActive(false);
	}

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("StartFromSelection: новое состояние из %d клеток (запомнено как точка возврата для R)"), InitialStateCells.Num());
}

void AAutomataOrchestrator::ResetToInitialState()
{
	if (InitialStateCells.Num() == 0)
	{
		// StartFromSelection() ещё ни разу не вызывался в этой сессии (и
		// файл не загружался) - нет сохранённой точки возврата, ведём себя
		// как раньше.
		GenerateRandom();
		if (GamePC)
		{
			GamePC->FrameAllCells(this);
		}
		return;
	}

	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("ResetToInitialState: фоновый шаг StepAsync() ещё считается - подождите его завершения"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("ResetToInitialState: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (AgeMaterials.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("ResetToInitialState: AgeMaterials пуст - назначьте хотя бы один материал в Details panel"));
		return;
	}

	// Новый прогон убирает запечённый меш-снимок, если он был (см.
	// BakeCellsToMesh()) - как и в GenerateRandom().
	ClearBakedMesh();
	ClearGhostShape();

	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	SelectedCells.Reset();
	ResetGenerationCounter();

	for (const FIntVector& Cell : InitialStateCells)
	{
		Grid->SetAlive(Cell, true);
		Grid->SetAge(Cell, 0);
	}

	RenderGridImmediate();

	// Камера сама кадрируется на результат - как хоткей Home.
	if (GamePC)
	{
		GamePC->FrameAllCells(this);
	}

	UE_LOG(LogTemp, Log, TEXT("ResetToInitialState: сетка восстановлена из сохранённой точки возврата (%d клеток)"), Grid->Num());
}

FString AAutomataOrchestrator::EnsureSaveDirectory() const
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AutomataSaves"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	return Dir;
}

FAutomatonSaveHeader AAutomataOrchestrator::BuildSaveHeader() const
{
	FAutomatonSaveHeader Header;
	Header.BirthCounts = BirthCounts;
	Header.SurvivalCounts = SurvivalCounts;
	Header.Neighborhood = Neighborhood;
	// CellSize - из сетки, не из UPROPERTY: сетка могла быть создана со
	// старым значением, а файл фиксирует её фактическую геометрию.
	Header.CellSize = Grid ? Grid->GetCellSize() : CellSize;
	Header.ChunkSize = ChunkSize;
	Header.GridSize = GridSize;
	Header.Seed = Seed;
	Header.Amount = Amount;
	Header.SpawnRadius = SpawnRadius;
	Header.ClusterFactor = ClusterFactor;
	// Header.CellCount выставит WriteSave() из фактического набора клеток.
	return Header;
}

void AAutomataOrchestrator::ApplySaveHeader(const FAutomatonSaveHeader& Header)
{
	// JSON-шапка правится руками в текстовом редакторе - значениям нельзя
	// доверять, клампы повторяют ClampMin соответствующих UPROPERTY.
	BirthCounts = Header.BirthCounts;
	SurvivalCounts = Header.SurvivalCounts;
	Neighborhood = Header.Neighborhood;
	CellSize = FMath::Max(1.0f, Header.CellSize);
	ChunkSize = FMath::Max(1, Header.ChunkSize);
	GridSize.X = FMath::Max(1, Header.GridSize.X);
	GridSize.Y = FMath::Max(1, Header.GridSize.Y);
	GridSize.Z = FMath::Max(1, Header.GridSize.Z);
	Seed = Header.Seed;
	Amount = FMath::Max(1, Header.Amount);
	SpawnRadius = FMath::Max(1, Header.SpawnRadius);
	ClusterFactor = FMath::Clamp(Header.ClusterFactor, 0.0f, 1.0f);
}

bool AAutomataOrchestrator::CaptureThumbnailPng(TArray64<uint8>& OutPngBytes) const
{
	OutPngBytes.Reset();

	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureThumbnailPng: GameViewport недоступен (не PIE?) - миниатюра пропущена"));
		return false;
	}

	FViewport* Viewport = GEngine->GameViewport->Viewport;
	const FIntPoint ViewportSize = Viewport->GetSizeXY();
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureThumbnailPng: нулевой размер вьюпорта - миниатюра пропущена"));
		return false;
	}

	TArray<FColor> RawPixels;
	if (!Viewport->ReadPixels(RawPixels) || RawPixels.Num() != ViewportSize.X * ViewportSize.Y)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureThumbnailPng: ReadPixels не удался - миниатюра пропущена"));
		return false;
	}

	// Альфа бэкбуфера не несёт полезного смысла для миниатюры (часто 0) -
	// принудительно делаем непрозрачным, иначе PNG вышел бы прозрачным.
	for (FColor& Pixel : RawPixels)
	{
		Pixel.A = 255;
	}

	// Обрезаем до квадрата ПО ЦЕНТРУ: короткая сторона вьюпорта берётся
	// целиком, длинная обрезается симметрично по краям - никакого искажения
	// пропорций, просто теряются края кадра. Строка за строкой memcpy прямо
	// из RawPixels, отдельный проход resize не нужен для самой обрезки.
	const int32 CropSize = FMath::Min(ViewportSize.X, ViewportSize.Y);
	const int32 CropOffsetX = (ViewportSize.X - CropSize) / 2;
	const int32 CropOffsetY = (ViewportSize.Y - CropSize) / 2;

	TArray<FColor> CroppedPixels;
	CroppedPixels.SetNumUninitialized(CropSize * CropSize);
	for (int32 Row = 0; Row < CropSize; ++Row)
	{
		const FColor* SrcRow = RawPixels.GetData() + (CropOffsetY + Row) * ViewportSize.X + CropOffsetX;
		FColor* DstRow = CroppedPixels.GetData() + Row * CropSize;
		FMemory::Memcpy(DstRow, SrcRow, CropSize * sizeof(FColor));
	}

	// Безусловное масштабирование (не только "если больше") - сторона
	// квадрата всегда становится РОВНО ThumbnailSizePixels, независимо от
	// текущего разрешения вьюпорта: единый стандартный размер миниатюры.
	TArray<FColor> ResizedPixels;
	const TArray<FColor>* PixelsToEncode = &CroppedPixels;
	int32 EncodeSize = CropSize;

	if (CropSize != ThumbnailSizePixels)
	{
		EncodeSize = FMath::Max(1, ThumbnailSizePixels);
		FImageUtils::ImageResize(CropSize, CropSize, CroppedPixels, EncodeSize, EncodeSize,
			ResizedPixels, /*bResizeSRGBinLinearSpace=*/true, /*bForceOpaqueOutput=*/true);
		PixelsToEncode = &ResizedPixels;
	}

	FImageUtils::PNGCompressImageArray(EncodeSize, EncodeSize,
		TArrayView64<const FColor>(PixelsToEncode->GetData(), PixelsToEncode->Num()), OutPngBytes);

	if (OutPngBytes.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureThumbnailPng: PNG-кодирование дало пустой результат - миниатюра пропущена"));
		return false;
	}

	return true;
}

bool AAutomataOrchestrator::WriteStateToFile(const FString& FilePath)
{
	// Сохраняем ИЗНАЧАЛЬНЫЙ паттерн (InitialStateCells) - НЕ трогая Grid:
	// ни сброса, ни перерисовки, ни движения камеры. InitialStateCells
	// заполняется либо StartFromSelection() (Enter), либо
	// LoadStateFromFile() - обе строго на game thread, так что читать этот
	// массив здесь безопасно без bStepInProgress guard'а (в отличие от
	// LoadStateFromFile(), эта функция не свапает Grid вовсе).
	if (InitialStateCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("WriteStateToFile: нет изначального паттерна для сохранения - сначала извлеките выделение через Enter или загрузите файл"));
		return false;
	}

	// Миниатюра - скриншот ТЕКУЩЕГО вида (какая сейчас камера, какая сейчас
	// живая симуляция на экране), а не сохраняемого паттерна - см.
	// doc-comment в заголовке. Косметика: неудача не прерывает сохранение
	// (CaptureThumbnailPng() сама логирует причину и зануляет буфер).
	const double CaptureStartSeconds = FPlatformTime::Seconds();
	TArray64<uint8> ThumbnailPng;
	CaptureThumbnailPng(ThumbnailPng);
	const double CaptureSeconds = FPlatformTime::Seconds() - CaptureStartSeconds;

	// Cells строится напрямую из InitialStateCells (возраст 0) - Grid тут
	// вообще не участвует.
	TArray<AutomatonStateSerializer::FSavedCell> Cells;
	Cells.Reserve(InitialStateCells.Num());
	for (const FIntVector& Cell : InitialStateCells)
	{
		AutomatonStateSerializer::FSavedCell& Saved = Cells.AddDefaulted_GetRef();
		Saved.Cell = Cell;
		Saved.Age = 0;
	}
	const FAutomatonSaveHeader Header = BuildSaveHeader();

	const double WriteStartSeconds = FPlatformTime::Seconds();
	TArray64<uint8> Bytes;
	// InitialStateCells пишется и как основной снимок, и как отдельная
	// InitialCells-секция (см. doc-comment namespace'а в
	// AutomatonStateSerializer.h) - это один и тот же набор клеток; формат
	// при этом не меняется (совместимость со старыми файлами, где секции
	// хранили разные вещи).
	if (!AutomatonStateSerializer::WriteSave(Header, Cells, InitialStateCells, ThumbnailPng, Bytes))
	{
		return false; // причина уже в логе
	}
	if (!FFileHelper::SaveArrayToFile(Bytes, *FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("WriteStateToFile: не удалось записать файл %s"), *FilePath);
		return false;
	}
	const double WriteSeconds = FPlatformTime::Seconds() - WriteStartSeconds;

	// Sibling .png - то, что реально даёт значок-превью в Проводнике (COM
	// IThumbnailProvider для расширения .casave сознательно не делается -
	// непропорциональная инженерия для этого проекта). Те же уже
	// закодированные байты, без повторного кодирования. Отказ - тоже
	// warn-and-continue: сам .casave уже успешно записан.
	if (ThumbnailPng.Num() > 0)
	{
		const FString ThumbnailPath = FPaths::SetExtension(FilePath, TEXT("png"));
		if (!FFileHelper::SaveArrayToFile(ThumbnailPng, *ThumbnailPath))
		{
			UE_LOG(LogTemp, Warning, TEXT("WriteStateToFile: не удалось записать миниатюру %s (сохранение .casave прошло успешно)"), *ThumbnailPath);
		}
	}

	LastSaveFilePath = FilePath;

	UE_LOG(LogTemp, Log, TEXT("WriteStateToFile: %d клеток (миниатюра: %lld байт) -> %s (%.1f КБ; скриншот: %.2f мс, запись: %.2f мс)"),
		Cells.Num(), ThumbnailPng.Num(), *FilePath, Bytes.Num() / 1024.0, CaptureSeconds * 1000.0, WriteSeconds * 1000.0);
	return true;
}

void AAutomataOrchestrator::SaveState()
{
	if (LastSaveFilePath.IsEmpty())
	{
		// Некуда тихо перезаписывать - первый Ctrl+S в сессии ведёт себя как
		// Ctrl+Shift+S и спрашивает путь один раз.
		SaveStateAs();
		return;
	}

	WriteStateToFile(LastSaveFilePath);
}

void AAutomataOrchestrator::SaveStateAs()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogTemp, Warning, TEXT("SaveStateAs: системные диалоги выбора файла недоступны"));
		return;
	}

	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	const FString DefaultFileName = FString::Printf(TEXT("Automaton_%s.casave"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	TArray<FString> PickedFiles;
	const bool bPicked = DesktopPlatform->SaveFileDialog(
		ParentWindowHandle,
		TEXT("Сохранить состояние автомата"),
		EnsureSaveDirectory(),
		DefaultFileName,
		TEXT("Automaton Save (*.casave)|*.casave"),
		EFileDialogFlags::None,
		PickedFiles);
	if (!bPicked || PickedFiles.Num() == 0)
	{
		// Отмена диалога - намерение пользователя, не ошибка.
		UE_LOG(LogTemp, Log, TEXT("SaveStateAs: отменено пользователем"));
		return;
	}

	FString FilePath = FPaths::ConvertRelativePathToFull(PickedFiles[0]);
	if (FPaths::GetExtension(FilePath).IsEmpty())
	{
		FilePath += TEXT(".casave");
	}

	WriteStateToFile(FilePath);
}

void AAutomataOrchestrator::LoadStateFromFile()
{
	// Загрузка несовместима с идущей симуляцией - останавливаем Play и
	// автошаг Shift+F, как в BakeCellsToMesh(). Stop() сам довершает
	// чанковый разлив; защитный вызов ниже покрывает разлив, идущий вне Play
	// (после Next() Play уже нет, а Tick ещё досыпает инстансы) - иначе его
	// хвост досыпал бы старые инстансы поверх загруженного состояния.
	if (bSimulationRunning)
	{
		Stop();
	}
	if (bFastStepActive)
	{
		StopFastStep();
	}
	FinishChunkedRenderImmediately();

	// Загрузка СВАПАЕТ Grid, который фоновый шаг может читать в этот момент -
	// guard обязателен (в отличие от SaveState()/SaveStateAs(), см. их
	// doc-comment).
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: фоновый шаг ещё считается - подождите и нажмите Ctrl+O ещё раз"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (AgeMaterials.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: AgeMaterials пуст - назначьте хотя бы один материал в Details panel"));
		return;
	}

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: системные диалоги выбора файла недоступны"));
		return;
	}

	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	TArray<FString> PickedFiles;
	const bool bPicked = DesktopPlatform->OpenFileDialog(
		ParentWindowHandle,
		TEXT("Загрузить состояние автомата"),
		EnsureSaveDirectory(),
		TEXT(""),
		TEXT("Automaton Save (*.casave)|*.casave"),
		EFileDialogFlags::None,
		PickedFiles);
	if (!bPicked || PickedFiles.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("LoadStateFromFile: отменено пользователем"));
		return;
	}

	const FString FilePath = FPaths::ConvertRelativePathToFull(PickedFiles[0]);
	TArray64<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: не удалось прочитать файл %s"), *FilePath);
		return;
	}

	FAutomatonSaveHeader Header;
	TArray<AutomatonStateSerializer::FSavedCell> Cells;
	TArray<FIntVector> LoadedInitialCells;
	TArray64<uint8> LoadedThumbnailPng;
	if (!AutomatonStateSerializer::ReadSave(Bytes, Header, Cells, LoadedInitialCells, LoadedThumbnailPng))
	{
		// Причина уже в логе. До этой точки никакое состояние оркестратора не
		// тронуто - отказ по битому/чужому файлу полностью безопасен.
		return;
	}

	// Порядок обязателен: сначала параметры (CreateGrid() читает живые
	// CellSize/ChunkSize), потом сетка.
	ApplySaveHeader(Header);
	ClearBakedMesh();
	ClearGhostShape();
	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	SelectedCells.Reset();
	ResetGenerationCounter();

	const double ApplyStartSeconds = FPlatformTime::Seconds();
	AutomatonStateSerializer::ApplyCells(Cells, *Grid);
	const double ApplySeconds = FPlatformTime::Seconds() - ApplyStartSeconds;

	// Точка возврата R - из ФАЙЛА (LoadedInitialCells), а не заново выведена
	// из загруженного снимка: файл хранит их раздельно именно для этого (см.
	// AutomatonStateSerializer.h) - R после загрузки должен вернуть к тому
	// же изначальному паттерну, что и до сохранения, а не к уже
	// проэволюционировавшему снимку. R реиграет с возрастами 0, как обычно;
	// точные возрасты снимка - повторный Ctrl+O.
	InitialStateCells = MoveTemp(LoadedInitialCells);

	// Последующий Ctrl+S тихо перезапишет именно этот файл.
	LastSaveFilePath = FilePath;

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("LoadStateFromFile: %d клеток из %s (заливка: %.2f мс); точка возврата для R - %d клеток; миниатюра в файле: %lld байт (пока не используется - задел под будущий UI со списком сохранений)"),
		Cells.Num(), *FilePath, ApplySeconds * 1000.0, InitialStateCells.Num(), LoadedThumbnailPng.Num());
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

	// Новый прогон убирает запечённый меш-снимок, если он был (см.
	// BakeCellsToMesh()) - иначе новые клетки рисовались бы сквозь него.
	ClearBakedMesh();
	ClearGhostShape();

	// GenerateRandom() всегда генерирует новое состояние с нуля и подхватывает
	// актуальный CellSize, если его поменяли в Details panel
	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	// Новый прогон - новый отсчёт поколений для HUD (см. GenerationCount/
	// FHudStats/ResetGenerationCounter()).
	ResetGenerationCounter();
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

	// Свежесгенерированное состояние - тоже валидная "точка возврата" R и
	// то, что уйдёт в файл при Save (см. doc-comment InitialStateCells) -
	// ровно как после StartFromSelection()/LoadStateFromFile(). Берём
	// фактически осевшие в сетке клетки (не сырое количество попыток
	// Amount - reject-sampling выше может давать коллизии в одну и ту же
	// клетку). Каждый повторный вызов GenerateRandom() (в т.ч. NewSeed())
	// перезаписывает точку возврата новой генерацией - это и есть "явный
	// запрос нового случайного состояния", просто теперь запоминаемого, а
	// не отбрасываемого в пустоту.
	Grid->GetAliveCells(InitialStateCells);

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
	FCellularAutomatonRule AutomatonRule(BirthCounts, SurvivalCounts, Neighborhood);
	TUniquePtr<FCellularAutomatonComputeStrategy> ComputeStrategy = CreateComputeStrategy();

	// Ручной шаг считает StepsPerRender поколений за одно нажатие (то же
	// значение, что крутится хоткеями T/G) и рендерит только итоговое -
	// промежуточные поколения на экран не попадают, ровно как поколения,
	// пропускаемые StepsPerRender'ом в непрерывном Play. При
	// StepsPerRender == 1 поведение прежнее: один шаг - один рендер.
	const int32 NumSteps = FMath::Max(1, StepsPerRender);

	// Счёт уходит в фоновый пул потоков, как и в StepAsync() - раньше Next()
	// считал синхронно на game thread, и с NumSteps > 1 нажатие F замораживало
	// экран на всё время счёта (в Play такого нет именно потому, что там счёт
	// фоновый). Промежуточные буферы поколений создаются уже в фоне, поэтому
	// CellSize/ChunkSize (UPROPERTY, могут править в Details panel) снимаем
	// здесь - фоновый поток не должен их читать.
	const float CellSizeSnapshot = CellSize;
	const int32 ChunkSizeSnapshot = ChunkSize;

	bStepInProgress = true;

	FCellGrid* CurrentGridPtr = Grid.Get();
	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	// Сырой указатель на *Grid без защиты времени жизни - как и в StepAsync(),
	// EndPlay() дожидается PendingStepFuture перед разрушением актора, а все
	// остальные пути замены Grid отказываются работать при bStepInProgress.
	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[AutomatonRule = MoveTemp(AutomatonRule), ComputeStrategy = MoveTemp(ComputeStrategy),
		 CurrentGridPtr, WeakThis, NumSteps, CellSizeSnapshot, ChunkSizeSnapshot]() mutable
		{
			const double StepStartSeconds = FPlatformTime::Seconds();

			TUniquePtr<FCellGrid> ResultGrid;
			const FCellGrid* SourceGrid = CurrentGridPtr;
			for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
			{
				TUniquePtr<FCellGrid> NextGrid = MakeUnique<FDenseCellGrid>(CellSizeSnapshot, ChunkSizeSnapshot);
				ComputeStrategy->Step(*SourceGrid, *NextGrid, AutomatonRule);
				CellAging::ComputeAges(SourceGrid, *NextGrid);
				ResultGrid = MoveTemp(NextGrid);
				SourceGrid = ResultGrid.Get();
			}

			const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;

			// Снимаем ещё здесь, в фоновом потоке, пока ComputeStrategy жива -
			// она уничтожится вместе с этой лямбдой, дальше её не будет
			// (см. FHudStats::EstimatedGpuComputeUploadMB). Отражает только
			// ПОСЛЕДНИЙ из NumSteps шагов - для HUD-индикатора этого достаточно.
			const int64 ComputeUploadBytes = ComputeStrategy->GetLastComputeUploadBytes();

			// Grid/рендер трогаем только на game thread (см. StepAsync()).
			AsyncTask(ENamedThreads::GameThread, [WeakThis, ResultGrid = MoveTemp(ResultGrid), StepSeconds, NumSteps, ComputeUploadBytes]() mutable
			{
				AAutomataOrchestrator* StrongThis = WeakThis.Get();
				if (!StrongThis)
				{
					return;
				}

				StrongThis->Grid = MoveTemp(ResultGrid);
				StrongThis->SelectedCells.Reset();
				StrongThis->bStepInProgress = false;

				// NumSteps реально посчитанных поколений за одно нажатие F -
				// см. GenerationCount/FHudStats.
				StrongThis->GenerationCount += NumSteps;
				StrongThis->LastGpuComputeUploadBytes = ComputeUploadBytes;

				// Ghost Shape пересчитывается по своему отдельному интервалу
				// поколений - см. ApplyStepResult() и план "Ghost Shape".
				if (StrongThis->bEnableGhostShape)
				{
					StrongThis->GhostShapeGenerationsSinceRefresh += NumSteps;
					if (StrongThis->GhostShapeGenerationsSinceRefresh >= FMath::Max(1, StrongThis->GhostShapeRefreshInterval))
					{
						StrongThis->GhostShapeGenerationsSinceRefresh = 0;
						StrongThis->RefreshGhostShape();
					}
				}

				// Всегда немедленно и целиком, в отличие от ApplyStepResult() -
				// ручной шаг игнорирует и bEnableChunkedRender, и счётчик
				// StepsSinceLastRender (пропуск рендера здесь уже "прожит"
				// самим циклом NumSteps выше).
				StrongThis->RenderGridImmediate();

				UE_LOG(LogTemp, Log, TEXT("Next: живых клеток %d после %d шаг(ов) (счёт: %.2f мс [фоновый поток])"),
					StrongThis->Grid->Num(), NumSteps, StepSeconds * 1000.0);
			});
		});
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

			// Снимаем ещё здесь, пока ComputeStrategy жива (уничтожится вместе
			// с этой лямбдой) - см. FHudStats::EstimatedGpuComputeUploadMB.
			const int64 ComputeUploadBytes = ComputeStrategy->GetLastComputeUploadBytes();

			// Grid/рендер трогаем только на game thread - AsyncTask сюда и
			// маршрутизирует. WeakThis - на случай, если актор уничтожили
			// (например, level unload) пока фоновый Step() ещё считался.
			AsyncTask(ENamedThreads::GameThread, [WeakThis, NextGridBuffer = MoveTemp(NextGridBuffer), StepSeconds, ComputeUploadBytes]() mutable
			{
				if (AAutomataOrchestrator* StrongThis = WeakThis.Get())
				{
					StrongThis->ApplyStepResult(MoveTemp(NextGridBuffer), StepSeconds, ComputeUploadBytes);
				}
			});
		});
}

TArray<TArray<FIntVector>> AAutomataOrchestrator::BuildAgeBuckets()
{
	const int32 NumBuckets = AgeMaterials.Num();

	// Бакетируем живые клетки по возрасту: MaterialIndex = N-1-min(Age, N-1) -
	// последний материал массива достаётся самым молодым (Age=0) клеткам,
	// первый - клеткам, доживших до (N-1) эпох и старше (см. doc-comment
	// AgeMaterials).
	TArray<FIntVector> AliveCells;

	// Если включено и в уровне есть ARenderCullVolume - отсекаем клетки вне
	// его границ ДО бакетирования/построения трансформов (см. doc-comment
	// bEnableRenderCullVolume) - иначе (выключено или актёра нет) рендерим
	// всё как раньше.
	ARenderCullVolume* CullVolume = bEnableRenderCullVolume ? EnsureRenderCullVolume() : nullptr;
	if (CullVolume)
	{
		Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), AliveCells);
	}
	else
	{
		Grid->GetAliveCells(AliveCells);
	}

	TArray<TArray<FIntVector>> Buckets;
	Buckets.SetNum(NumBuckets);
	for (const FIntVector& Cell : AliveCells)
	{
		const uint8 Age = Grid->GetAge(Cell);
		const int32 MaterialIndex = NumBuckets - 1 - FMath::Min(static_cast<int32>(Age), NumBuckets - 1);
		Buckets[MaterialIndex].Add(Cell);
	}

	// Два разных вида числа тут (см. doc-comment FCellRenderStats):
	// RenderedCellCount/TotalCellCount - ПАРА, показывает масштаб расчётов
	// (сколько живых клеток реально отрисовано после отсечения
	// ARenderCullVolume против того, сколько их всего в сетке); а
	// EstimatedUploadMB - ОДНО общее число, оценка размера данных, которые
	// реально уходят в AddInstances() (не настоящий занятый VRAM - не
	// учитывает оверхед LOD-дерева HISM, ресурсы меша/материала, накладные
	// расходы драйвера, только сам TArray<FTransform>). Результат кладём в
	// LastRenderStats - UE_LOG ниже читает уже посчитанное оттуда, а не из
	// локальных переменных, чтобы будущий HUD (GetLastRenderStats()) видел
	// те же самые цифры, что и лог.
	LastRenderStats.RenderedCellCount = AliveCells.Num();
	LastRenderStats.TotalCellCount = Grid->Num();
	LastRenderStats.BytesPerInstance = (int32)sizeof(FTransform);
	LastRenderStats.EstimatedUploadMB = (double(LastRenderStats.RenderedCellCount) * LastRenderStats.BytesPerInstance) / (1024.0 * 1024.0);

	UE_LOG(LogTemp, Log, TEXT("BuildAgeBuckets: %d/%d живых клеток (отрисовано/всего) - выгрузка в AddInstances ~%.2f МБ (%d байт/инстанс, TArray<FTransform>, без учёта оверхеда HISM/драйвера)"),
		LastRenderStats.RenderedCellCount, LastRenderStats.TotalCellCount,
		LastRenderStats.EstimatedUploadMB, LastRenderStats.BytesPerInstance);

	return Buckets;
}

ARenderCullVolume* AAutomataOrchestrator::EnsureRenderCullVolume()
{
	if (!IsValid(CachedRenderCullVolume))
	{
		CachedRenderCullVolume = Cast<ARenderCullVolume>(UGameplayStatics::GetActorOfClass(GetWorld(), ARenderCullVolume::StaticClass()));
	}
	return CachedRenderCullVolume;
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

void AAutomataOrchestrator::ApplyStepResult(TUniquePtr<FCellGrid> NewGrid, double StepSeconds, int64 ComputeUploadBytes)
{
	Grid = MoveTemp(NewGrid);
	LastGpuComputeUploadBytes = ComputeUploadBytes;
	// Новое поколение делает старое выделение бессмысленным - сбрасываем
	// сразу, независимо от того, дойдёт ли дело до фактического рендера ниже
	// (см. doc-comment SelectedCells в заголовке).
	SelectedCells.Reset();

	// Сужено до конца фонового чтения Grid - дальше (рендер, возможный
	// чанковый "разлив") фонового потока уже не касается, так что следующий
	// StepAsync() может стартовать независимо от того, что происходит с
	// рендером ниже.
	bStepInProgress = false;

	// Одно реально посчитанное поколение - считаем для HUD независимо от
	// того, пропустит ли StepsSinceLastRender ниже фактический рендер этого
	// поколения (см. GenerationCount/FHudStats).
	++GenerationCount;

	// Ghost Shape пересчитывается по своему отдельному интервалу поколений,
	// независимо от StepsPerRender - см. план "Ghost Shape".
	if (bEnableGhostShape)
	{
		++GhostShapeGenerationsSinceRefresh;
		if (GhostShapeGenerationsSinceRefresh >= FMath::Max(1, GhostShapeRefreshInterval))
		{
			GhostShapeGenerationsSinceRefresh = 0;
			RefreshGhostShape();
		}
	}

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

void AAutomataOrchestrator::SetRenderCullVolumeEnabled(bool bEnabled)
{
	bEnableRenderCullVolume = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("SetRenderCullVolumeEnabled: отсечение по ARenderCullVolume %s"), bEnabled ? TEXT("включено") : TEXT("выключено"));
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::SetGhostShapeEnabled(bool bEnabled)
{
	bEnableGhostShape = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("SetGhostShapeEnabled: Ghost Shape %s"), bEnabled ? TEXT("включён") : TEXT("выключен"));

	// RefreshGhostShape() сам разберётся, что делать: bEnableGhostShape ==
	// false в его собственном guard'е сведётся к ClearGhostShape() - не
	// нужно дублировать эту ветку здесь. Счётчик сбрасываем всегда, чтобы
	// ручное включение сразу пересчитало силуэт, а не ждало остаток
	// GhostShapeRefreshInterval с прошлого раза.
	GhostShapeGenerationsSinceRefresh = 0;
	RefreshGhostShape();
}

void AAutomataOrchestrator::RefreshRenderCullVolume()
{
	if (!Grid)
	{
		// Ещё нет сетки (до первого GenerateRandom()) - перерисовывать
		// нечего, следующий GenerateRandom()/Next() и так учтёт актуальные
		// границы куба сам.
		return;
	}

	// В отличие от ApplyCellCullDistances() (который просто перевызывает
	// SetCullDistances() на уже построенных инстансах), изменение куба
	// меняет САМ набор клеток, попадающих в AddInstances - недостаточно
	// применить настройку "на лету" без полного набора инстансов, нужно
	// заново пройти BuildAgeBuckets()/AddInstances() для текущего состояния
	// (не считая новое поколение - RenderGridImmediate() рендерит уже
	// посчитанный Grid как есть, тот же путь, что Next()/GenerateRandom()).
	// Иначе переключение хоткеем C или перетаскивание ARenderCullVolume
	// визуально ничего не меняло бы до следующего шага симуляции - ровно
	// то же соображение, что и у SetCellCullingEnabled() выше.
	RenderGridImmediate();

	// Ghost Shape отсекает по тем же границам куба (см. RefreshGhostShape()) -
	// без этого вызова передвинутый/ресайзнутый куб оставлял бы старый
	// ghost-силуэт висеть до истечения GhostShapeRefreshInterval поколений
	// (могло потребовать "прокрутить несколько эпох", прежде чем форма
	// подстроится). PostEditMove(bFinished=true)/PostEditChangeProperty на
	// ARenderCullVolume уже сами по себе - естественный дебаунс: событие
	// приходит один раз по завершении перетаскивания/правки, а не на каждый
	// промежуточный тик драга.
	if (bEnableGhostShape)
	{
		GhostShapeGenerationsSinceRefresh = 0;
		RefreshGhostShape();
	}
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