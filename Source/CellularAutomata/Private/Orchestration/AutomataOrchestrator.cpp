// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Grid/SparseCellGrid.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "Automata/Simulation/CellularAutomatonRule.h"


// Sets default values
AAutomataOrchestrator::AAutomataOrchestrator()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = false;

	// Инстансированный меш для отрисовки клеток автомата - корневой компонент.
	// Клетки чисто визуальные, коллизия не нужна и только замедляет
	// добавление инстансов при большом их количестве.
	CellsMesh = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CellsMesh"));
	CellsMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RootComponent = CellsMesh;
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

void AAutomataOrchestrator::InitializeRenderer()
{
	if (!Renderer)
	{
		Renderer = MakeUnique<FInstancedMeshCellGridRenderer>(CellsMesh);
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
	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandom: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!CellsMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandom: CellsMesh компонент отсутствует"));
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

	if (!CellsMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: CellsMesh компонент отсутствует"));
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

void AAutomataOrchestrator::Start()
{
	UE_LOG(LogTemp, Log, TEXT("Start game"));
	Resume();
	//UiController->HideHUD();
}

void AAutomataOrchestrator::Pause()
{
	GamePC->SetCameraControlEnabled(false);
}
void AAutomataOrchestrator::Resume()
{
	GamePC->SetCameraControlEnabled(true);
}

void AAutomataOrchestrator::Stop()
{
	
}

void AAutomataOrchestrator::Clear()
{
	
}