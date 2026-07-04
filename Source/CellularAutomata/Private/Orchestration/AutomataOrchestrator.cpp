// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Grid/SparseCellGrid.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
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

	// Шаг считается асинхронно (см. StepAsync()) - пока предыдущий не
	// завершился, новый не запускаем (гонка на Grid), просто ждём. Раньше
	// здесь был while-цикл, "нагоняющий" пропущенные шаги за один тик - для
	// синхронного Next() это было безопасно, но для асинхронного шага
	// означало бы запуск нескольких фоновых Step() поверх друг друга.
	// Оставшееся время не копится "про запас" - реальная скорость сама
	// упрётся в то, сколько Step() занимает на этой сетке.
	if (TimeSinceLastStep >= StepInterval && !bStepInProgress)
	{
		TimeSinceLastStep = 0.0f;
		StepAsync();
	}
}

void AAutomataOrchestrator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
}

void AAutomataOrchestrator::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
}

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

void AAutomataOrchestrator::InitializeRenderer()
{
	UInstancedStaticMeshComponent* DesiredComponent = GetActiveCellsMeshComponent();

	if (!Renderer || Renderer->GetComponent() != DesiredComponent)
	{
		// CellMeshComponentType поменяли в Details panel - у ранее активного
		// компонента могли остаться инстансы с прошлого рендера, иначе
		// увидим оба набора кубов одновременно.
		if (UInstancedStaticMeshComponent* PreviousComponent = Renderer ? Renderer->GetComponent() : nullptr)
		{
			PreviousComponent->ClearInstances();
		}

		Renderer = MakeUnique<FInstancedMeshCellGridRenderer>(DesiredComponent);
	}
}

TUniquePtr<FCellGrid> AAutomataOrchestrator::CreateGrid() const
{
	switch (GridStorageStrategy)
	{
	case EGridStorageStrategy::Dense:
		return MakeUnique<FDenseCellGrid>(CellSize, ChunkSize);
	case EGridStorageStrategy::Sparse:
	default:
		return MakeUnique<FSparseCellGrid>(CellSize);
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

	InitializeRenderer();

	// GenerateRandom() всегда генерирует новое состояние с нуля и подхватывает
	// актуальный CellSize, если его поменяли в Details panel
	Grid = CreateGrid();

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

	Renderer->SetMesh(CellMesh);
	Renderer->SetMaterial(CellMaterial);

	const double RenderStartSeconds = FPlatformTime::Seconds();
	Renderer->Render(*Grid);
	const double RenderSeconds = FPlatformTime::Seconds() - RenderStartSeconds;
	const FRenderTimings& RT = Renderer->GetLastRenderTimings();

	UE_LOG(LogTemp, Log, TEXT("GenerateRandom: заспавнено %d клеток в радиусе %d (генерация: %.2f мс, отрисовка: %.2f мс [SetMesh/Material: %.2f, ClearInstances: %.2f, Scale: %.2f, GetAliveCells: %.2f, BuildTransforms: %.2f, AddInstances: %.2f])"),
		Grid->Num(), SpawnRadius, GenerationSeconds * 1000.0, RenderSeconds * 1000.0,
		RT.SetMeshSeconds * 1000.0, RT.ClearSeconds * 1000.0, RT.ScaleSeconds * 1000.0,
		RT.GetAliveSeconds * 1000.0, RT.BuildTransformsSeconds * 1000.0, RT.AddInstanceSeconds * 1000.0);
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

	InitializeRenderer();

	// Строим правило заново на каждый вызов, чтобы правки BirthCounts/
	// SurvivalCounts/Neighborhood в Details panel подхватывались немедленно
	// (аналогично тому, как GenerateRandom() каждый раз пересоздаёт Grid,
	// а не кэширует его)
	const FCellularAutomatonRule AutomatonRule(BirthCounts, SurvivalCounts, Neighborhood);

	TUniquePtr<FCellGrid> NextGrid = CreateGrid();

	const double StepStartSeconds = FPlatformTime::Seconds();
	AutomatonRule.Step(*Grid, *NextGrid);
	const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;

	Grid = MoveTemp(NextGrid);

	Renderer->SetMesh(CellMesh);
	Renderer->SetMaterial(CellMaterial);

	const double RenderStartSeconds = FPlatformTime::Seconds();
	Renderer->Render(*Grid);
	const double RenderSeconds = FPlatformTime::Seconds() - RenderStartSeconds;
	const FRenderTimings& RT = Renderer->GetLastRenderTimings();

	UE_LOG(LogTemp, Log, TEXT("Next: живых клеток %d после шага (шаг: %.2f мс, отрисовка: %.2f мс [SetMesh/Material: %.2f, ClearInstances: %.2f, Scale: %.2f, GetAliveCells: %.2f, BuildTransforms: %.2f, AddInstances: %.2f])"),
		Grid->Num(), StepSeconds * 1000.0, RenderSeconds * 1000.0,
		RT.SetMeshSeconds * 1000.0, RT.ClearSeconds * 1000.0, RT.ScaleSeconds * 1000.0,
		RT.GetAliveSeconds * 1000.0, RT.BuildTransformsSeconds * 1000.0, RT.AddInstanceSeconds * 1000.0);
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

	// Правило и буфер следующего поколения строим здесь, на game thread -
	// оба читают UPROPERTY (BirthCounts/SurvivalCounts/Neighborhood/CellSize/
	// ChunkSize/GridStorageStrategy), которые могут одновременно
	// редактироваться в Details panel. После этой точки фоновый поток их
	// больше не касается - только *Grid (на чтение) и NextGridBuffer (на
	// запись, свежесозданный, ни с кем не общий).
	FCellularAutomatonRule AutomatonRule(BirthCounts, SurvivalCounts, Neighborhood);
	TUniquePtr<FCellGrid> NextGridBuffer = CreateGrid();

	bStepInProgress = true;

	FCellGrid* CurrentGridPtr = Grid.Get();
	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[AutomatonRule = MoveTemp(AutomatonRule), NextGridBuffer = MoveTemp(NextGridBuffer), CurrentGridPtr, WeakThis]() mutable
		{
			const double StepStartSeconds = FPlatformTime::Seconds();
			AutomatonRule.Step(*CurrentGridPtr, *NextGridBuffer);
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

void AAutomataOrchestrator::ApplyStepResult(TUniquePtr<FCellGrid> NewGrid, double StepSeconds)
{
	Grid = MoveTemp(NewGrid);

	InitializeRenderer();

	Renderer->SetMesh(CellMesh);
	Renderer->SetMaterial(CellMaterial);

	if (bEnableChunkedRender)
	{
		// Разливаем AddInstances по нескольким Tick() вместо одного кадра
		// (см. AdvanceChunkedRender()), чтобы не блокировать game thread
		// (а с ним и камеру) на десятки-сотни миллисекунд при больших
		// сетках, даже когда сам шаг уже посчитан асинхронно. Включение/
		// выключение - только вручную (Details panel или хоткей Z), без
		// автоматического порога по числу клеток. bStepInProgress остаётся
		// true - AdvanceChunkedRender() сбросит его, когда рендер закончится.
		const double BeginRenderStartSeconds = FPlatformTime::Seconds();
		Renderer->BeginRender(*Grid);
		const double BeginRenderSeconds = FPlatformTime::Seconds() - BeginRenderStartSeconds;

		bChunkedRenderInProgress = true;
		ChunkedRenderStartSeconds = FPlatformTime::Seconds();
		ChunkedRenderFrameCount = 0;

		UE_LOG(LogTemp, Log, TEXT("StepAsync: живых клеток %d после шага (шаг: %.2f мс [фоновый поток], подготовка рендера: %.2f мс) - рендер разлит по кадрам (%d инстансов/кадр)"),
			Grid->Num(), StepSeconds * 1000.0, BeginRenderSeconds * 1000.0, ChunkedRenderCellsPerFrame);
		return;
	}

	const double RenderStartSeconds = FPlatformTime::Seconds();
	Renderer->Render(*Grid);
	const double RenderSeconds = FPlatformTime::Seconds() - RenderStartSeconds;
	const FRenderTimings& RT = Renderer->GetLastRenderTimings();

	UE_LOG(LogTemp, Log, TEXT("StepAsync: живых клеток %d после шага (шаг: %.2f мс [фоновый поток], отрисовка: %.2f мс [SetMesh/Material: %.2f, ClearInstances: %.2f, Scale: %.2f, GetAliveCells: %.2f, BuildTransforms: %.2f, AddInstances: %.2f])"),
		Grid->Num(), StepSeconds * 1000.0, RenderSeconds * 1000.0,
		RT.SetMeshSeconds * 1000.0, RT.ClearSeconds * 1000.0, RT.ScaleSeconds * 1000.0,
		RT.GetAliveSeconds * 1000.0, RT.BuildTransformsSeconds * 1000.0, RT.AddInstanceSeconds * 1000.0);

	bStepInProgress = false;
}

void AAutomataOrchestrator::SetChunkedRenderEnabled(bool bEnabled)
{
	bEnableChunkedRender = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("SetChunkedRenderEnabled: рендер по кадрам %s"), bEnabled ? TEXT("включён") : TEXT("выключен"));
}

void AAutomataOrchestrator::AdvanceChunkedRender()
{
	++ChunkedRenderFrameCount;

	const bool bMoreRemaining = Renderer->AdvanceRenderChunk(ChunkedRenderCellsPerFrame);
	if (bMoreRemaining)
	{
		return;
	}

	bChunkedRenderInProgress = false;
	bStepInProgress = false;

	const double TotalSeconds = FPlatformTime::Seconds() - ChunkedRenderStartSeconds;
	const FRenderTimings& RT = Renderer->GetLastRenderTimings();

	UE_LOG(LogTemp, Log, TEXT("StepAsync: рендер разлитый по кадрам завершён - живых клеток %d за %d кадр(ов)/%.2f мс (AddInstances суммарно: %.2f мс)"),
		Grid->Num(), ChunkedRenderFrameCount, TotalSeconds * 1000.0, RT.AddInstanceSeconds * 1000.0);
}

void AAutomataOrchestrator::Start()
{
	UE_LOG(LogTemp, Log, TEXT("Start game"));
	Resume();

	if (!Grid)
	{
		GenerateRandom();
	}

	TimeSinceLastStep = 0.0f;
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
	SetActorTickEnabled(false);
}

void AAutomataOrchestrator::Clear()
{
	
}