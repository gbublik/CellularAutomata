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
#include "GameFramework/FloatingPawnMovement.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/CellAging.h"
#include "Automata/Simulation/CellDecay.h"
#include "Automata/Simulation/RuleStringParser.h"
#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"
#include "Automata/Simulation/ComputeStrategy/GpuComputeStrategy.h"
#include "Automata/Selection/CellSelection.h"
#include "Ui/MainHudWidget.h"
#include "Automata/Capture/CellRasterizer.h"
#include "Automata/Generation/StateGenerators.h"
#include "Automata/Generation/StateGeneratorPresets.h"
#include "Automata/Meshing/CellMeshBuilder.h"
#include "Automata/Grid/GridDownsample.h"
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
#include "Automata/Rendering/RenderPresets.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "EngineUtils.h"

// Сглаженный FPS движка - определён в UnrealEngine.cpp, без публичного
// заголовка, объявляется локально там, где используется (тот же паттерн,
// что и в самом движке, см. напр. EngineAnalyticsSessionSummary.cpp) - см.
// FHudStats::CurrentFPS в Tick().
extern ENGINE_API float GAverageFPS;

namespace
{
	/** Печатает разбивку FRenderTimings по фазам. До этого все шесть таймеров
	 *  исправно набирались и молча выбрасывались - GetLastRenderTimings() не
	 *  звал никто, и ChunkedRenderCellsPerFrame подбирался на глаз по
	 *  заиканиям.
	 *
	 *  Фазы намеренно разделены на две группы, а не сложены в одну сумму:
	 *  Transforms/AddInstances/CustomData набираются ПОЧАНКОВО и потому
	 *  размазаны по кадрам разлива - только из них имеет смысл считать
	 *  цену клетки и цену кадра, т.е. ровно те две величины, по которым
	 *  подбирается ChunkedRenderCellsPerFrame. SetMesh/Clear/Scale/Reorder
	 *  случаются РАЗОМ внутри BeginRender() на первом кадре и по кадрам не
	 *  делятся вовсе, так что усреднять их вместе с остальными - значит
	 *  занижать первый кадр и завышать все следующие. Reorder тут особенно
	 *  интересен: Algo::Sort по миллионам инстансов в один поток может
	 *  оказаться дороже всего разлива, и никакой бюджет на кадр его не
	 *  размажет.
	 *
	 *  RenderedCells - это LastRenderStats.RenderedCellCount, т.е. число
	 *  клеток ПОСЛЕ отсечения кубом (ARenderCullVolume): именно оно уходит в
	 *  AddInstances и именно им управляет ChunkedRenderCellsPerFrame, а не
	 *  Grid->Num(). */
	void LogRenderTimings(const TCHAR* Context, const FRenderTimings& Timings, int32 RenderedCells, int32 FrameCount)
	{
		const double ChunkedSeconds = Timings.BuildTransformsSeconds + Timings.AddInstanceSeconds + Timings.CustomDataSeconds;
		const double SetupSeconds = Timings.SetMeshSeconds + Timings.ClearSeconds + Timings.ScaleSeconds + Timings.ReorderSeconds;

		const int32 SafeFrameCount = FMath::Max(FrameCount, 1);
		const double PerCellMicroseconds = (RenderedCells > 0) ? ChunkedSeconds * 1000000.0 / RenderedCells : 0.0;
		const double PerFrameMs = ChunkedSeconds * 1000.0 / SafeFrameCount;

		// FPS в той же строке, потому что вставка инстансов и их отрисовка -
		// разные стороны одного выбора: HISM строит дерево кластеров (дороже
		// вставка), но получает окклюзию и отсечение по кластерам (дешевле
		// отрисовка). Сравнивать CellMeshComponentType по одному лишь
		// AddInstances бессмысленно, а на глаз - тем более.
		UE_LOG(LogTemp, Log, TEXT("RenderTimings[%s]: клеток %d | FPS %.1f | разлив %.2f мс = %.3f мкс/клетка, %.2f мс/кадр за %d кадр(ов) [Transforms %.2f / AddInstances %.2f / CustomData %.2f] | разово в BeginRender %.2f мс [SetMesh %.2f / Clear %.2f / Scale %.2f / Reorder %.2f]"),
			Context, RenderedCells, GAverageFPS,
			ChunkedSeconds * 1000.0, PerCellMicroseconds, PerFrameMs, SafeFrameCount,
			Timings.BuildTransformsSeconds * 1000.0, Timings.AddInstanceSeconds * 1000.0, Timings.CustomDataSeconds * 1000.0,
			SetupSeconds * 1000.0,
			Timings.SetMeshSeconds * 1000.0, Timings.ClearSeconds * 1000.0,
			Timings.ScaleSeconds * 1000.0, Timings.ReorderSeconds * 1000.0);
	}
}

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

	// PIE дублирует актор из редакторского мира вместе с текущим СОСТОЯНИЕМ
	// компонентов - если пользователь нажал GenerateRandom()/Next() прямо в
	// редакторе перед запуском игры, реальные инстансы на компонентах
	// дублируются в PIE-копию. Штатный путь (RenderGridImmediate()) чистит
	// не всё (см. doc-comment ClearAllCellInstances()) - чистим здесь явно
	// и безусловно, до того как что-либо ещё успеет вызвать рендер.
	ClearAllCellInstances();
	ClearBakedMesh();
	ClearGhostShape();

	InitializePlayerController();
	EnsureCellsRenderer();
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
	UpdateHudStats();

	// Разлитый по кадрам рендер (см. bEnableChunkedRender) продолжается
	// независимо от bSimulationRunning - если игру остановили посреди
	// "разлива", он всё равно должен доехать до конца, а не застрять
	// наполовину отрисованным.
	if (bChunkedRenderInProgress)
	{
		AdvanceChunkedRender();
	}

	// Срез привязан к камере, значит при полёте его надо перестраивать. Здесь,
	// ДО всех ранних возвратов ниже: разглядывают структуру обычно на паузе,
	// когда ни bSimulationRunning, ни bFastStepActive не выставлены. Именно
	// ради этого SetViewSliceEnabled() и включает тик сам (см. там), иначе
	// актор на паузе не тикает вовсе и срез бы застыл.
	// Не трогаем сетку, пока её читает фоновый шаг или дорисовывает чанковый
	// разлив - те же гварды, что у всех прочих путей рендера.
	if (!bStepInProgress && !bChunkedRenderInProgress && ShouldRefreshViewSlice())
	{
		RefreshRenderCullVolume();
	}

	// Автошаг Shift+F (см. StartFastStep()) - взаимоисключающ с
	// bSimulationRunning (Start() отказывает, пока это активно, и наоборот),
	// поэтому безопасно делить TimeSinceLastStep с обычным Play.
	// Пока включён "ждать разлив" (см. bWaitForChunkedRenderToFinish), не
	// запускаем следующий шаг, пока предыдущий чанковый "разлив" ещё
	// рисуется - AdvanceChunkedRender() выше в этом же Tick() уже мог его
	// как раз завершить, так что проверка сразу актуальна для этого кадра.
	const bool bBlockedByChunkedRender = bWaitForChunkedRenderToFinish && bChunkedRenderInProgress;

	// Интервал между фоновыми заходами: не 1/Speed, а (поколений за заход)/Speed -
	// иначе, когда заход считает пачку из StepsPerRender поколений (см.
	// BatchGenerations в StepAsync()), реальная частота поколений выросла бы в
	// StepsPerRender раз, и Speed начал бы означать "заходов в секунду" вместо
	// "поколений в секунду". LastDispatchGenerations - то, что последний
	// StepAsync() решил считать за заход (1, пока пачки не включились), так что
	// первый заход прогона паcуется как раньше, а дальше интервал сходится к
	// фактическому размеру пачки.
	const float GenerationsPerDispatch = float(FMath::Max(1, LastDispatchGenerations));

	if (bFastStepActive)
	{
		TimeSinceLastStep += DeltaTime;
		const float StepInterval = GenerationsPerDispatch / FMath::Max(Speed, KINDA_SMALL_NUMBER);

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
	const float StepInterval = GenerationsPerDispatch / FMath::Max(Speed, KINDA_SMALL_NUMBER);

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

void AAutomataOrchestrator::UpdateHudStats()
{
	LastHudStats.bIsComputing = bStepInProgress;
	LastHudStats.bIsRendering = bChunkedRenderInProgress;
	LastHudStats.CurrentFPS = GAverageFPS;
	LastHudStats.GenerationCount = GenerationCount;
	LastHudStats.EstimatedGpuComputeUploadMB = LastGpuComputeUploadBytes / (1024.0 * 1024.0);
	// Grid->Num() - готовый счётчик в чанках, не скан сетки (см. FDenseCellGrid),
	// так что читать его дёшево даже на миллионах клеток.
	LastHudStats.AliveCellCount = Grid.IsValid() ? Grid->Num() : 0;
	LastHudStats.SelectedCellCount = SelectedCells.Num();

	// Заданные настройки - просто зеркалим, чтобы HUD читал ровно то же, что
	// крутят хоткеи (+/- и T/G) и Details panel, а не хранил свою копию.
	LastHudStats.SimulationSpeed = Speed;
	LastHudStats.StepsPerRender = StepsPerRender;

	// Скорость камеры - фактическая, с учётом удержания Shift (см. doc-comment
	// FHudStats::CameraSpeed).
	LastHudStats.CameraSpeed = 0.0f;
	if (GamePC)
	{
		if (const APawn* FlyingPawn = GamePC->GetPawn())
		{
			if (const UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(FlyingPawn->GetMovementComponent()))
			{
				LastHudStats.CameraSpeed = Movement->MaxSpeed;
			}
		}
	}

	// Режимы работы - зеркала живых переключателей (см. блок режимов в
	// FHudStats). Все дёшевы: голые чтения полей, никаких сканов сетки.
	LastHudStats.bSimulationRunning = bSimulationRunning;
	LastHudStats.bFastStepActive = bFastStepActive;
	LastHudStats.bSelectionModeActive = GamePC ? GamePC->IsSelectionModeActive() : false;
	LastHudStats.bOrthographicCamera = GamePC ? GamePC->IsOrthographicCamera() : false;
	LastHudStats.ComputeMethod = ComputeMethod;
	LastHudStats.bChunkedRenderEnabled = bEnableChunkedRender;
	LastHudStats.ChunkedRenderOrder = ChunkedRenderOrder;
	LastHudStats.bWaitForChunkedRenderToFinish = bWaitForChunkedRenderToFinish;
	LastHudStats.bCellCullingEnabled = bEnableCellCulling;
	LastHudStats.bRenderCullVolumeEnabled = bEnableRenderCullVolume;
	LastHudStats.bGhostShapeEnabled = bEnableGhostShape;
	LastHudStats.bCellsCastShadows = bCellsCastShadows;
	LastHudStats.bBackgroundVisible = bShowBackground;

	// Профиль рендера: индекс, имя и признак "после применения что-то крутили
	// руками" - см. FRenderPreset/ApplyRenderPreset().
	LastHudStats.RenderPresetIndex = ActiveRenderPresetIndex;
	LastHudStats.RenderPresetName = GetActiveRenderPresetName();
	LastHudStats.bRenderPresetModified = bRenderPresetModified;

	// Генератор начального состояния - зеркало GenerationParams плюс оценка
	// объёма. Имя дёшево (switch по перечислению), а вот оценка у шаров
	// считает решёточные точки точно, за O(R^2) - при радиусе в сотни клеток
	// это миллионы операций, которым нечего делать в каждом кадре. Пересчёт
	// раз в четверть секунды: тот же приём, что у GenerationsPerSecond и у
	// троттлинга диагностики отсечения.
	LastHudStats.StateGeneratorName = StateGenerators::GetDisplayName(GenerationParams.Type);

	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds - LastGeneratorEstimateSeconds >= 0.25)
	{
		LastGeneratorEstimateSeconds = NowSeconds;
		LastHudStats.EstimatedGeneratorCells = StateGenerators::EstimateCellCount(GenerationParams);
	}

	// Куб: "включено" и "работает" - разные вещи (актёра может не быть на
	// уровне, куб может быть спрятан), поэтому три отдельных поля, и итоговое
	// берётся из того же GetActiveCullVolume(), что и весь рендер - HUD не
	// повторяет условие своей копией, иначе они могли бы разойтись.
	const ARenderCullVolume* AnyCullVolume = EnsureRenderCullVolume();
	LastHudStats.bRenderCullVolumeVisible = AnyCullVolume && AnyCullVolume->IsVolumeVisible();
	LastHudStats.bCullVolumeActive = GetActiveCullVolume() != nullptr;
	LastHudStats.bGhostShapeReplacesDetailedRender = ShouldGhostShapeReplaceDetailedRender();

	UpdateGenerationsPerSecond();
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
	EnsureCellsRenderer();
	EnsureSelectionMeshComponent();
}

#if WITH_EDITOR
void AAutomataOrchestrator::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellMeshComponentType))
	{
		EnsureCellsRenderer();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellMaterial)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, AgeColors)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, AgeColorMaxAge)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, DecayColors)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, SelectionColor))
	{
		// Цвет - чистая функция уже посчитанного состояния, ждать следующего
		// поколения незачем (а на паузе его и не будет). Перерисовываем
		// текущее состояние на месте: только если сетка есть и фоновый шаг
		// её сейчас не читает - RenderGridImmediate() иначе гонялся бы с ним
		// за Grid, ровно как и все прочие пути, трогающие сетку.
		if (Grid && !bStepInProgress)
		{
			RenderGridImmediate();
		}
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellCullStartDistance)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellCullEndDistance))
	{
		// Без этого правка чисел в Details panel (в т.ч. во время PIE) не
		// применялась вплоть до следующего фактического рендера (шага
		// симуляции) или переключения хоткеем B (SetCellCullingEnabled()
		// зовёт ApplyCellCullDistances() сама, немедленно) - на паузе или
		// между шагами значение выглядело "зафиксированным" и не
		// реагирующим на CellCullEndDistance, хотя число менялось честно.
		// ApplyCellCullDistances() безопасно звать когда угодно - трогает
		// только CellsMeshHierarchical/CellsMeshFlat/
		// SelectionMeshComponent (все существуют с конструктора) и
		// GamePC/Grid только под null-проверками для диагностического лога.
		ApplyCellCullDistances();
	}
}
#endif

void AAutomataOrchestrator::NewSeed()
{
	if (bStepInProgress)
	{
		// Не отказываем молча (GenerateRandom() ниже всё равно откажется -
		// фоновый поток читает *Grid), а откладываем до завершения шага, как
		// это делает R - см. doc-comment bNewSeedPending. Seed намеренно НЕ
		// перекатывается здесь: он должен смениться ровно один раз, в момент
		// фактического реролла, иначе несколько отложенных нажатий сожгли бы
		// несколько сидов, а показали бы только последний.
		bNewSeedPending = true;
		bResetToInitialStatePending = false;
		UE_LOG(LogTemp, Warning, TEXT("NewSeed: фоновый шаг StepAsync() ещё считается - новый сид отложен до его завершения"));
		return;
	}

	Seed = FMath::Rand();
	GenerateRandom();
}

void AAutomataOrchestrator::ApplyRuleString()
{
	// Целиком делегирует TryApplyRuleString() - единственный путь применения
	// правила строкой (кнопка в Details panel, поле в HUD и пресеты приходят
	// сюда же), чтобы семантика "что именно и в каком порядке присваивается"
	// жила ровно в одном месте.
	FString Error;
	TryApplyRuleString(RuleString, Error);
}

bool AAutomataOrchestrator::TryApplyRuleString(const FString& InRuleString, FString& OutError)
{
	RuleStringParser::FParsedRule Parsed;
	if (!RuleStringParser::ParseRuleString(InRuleString, Parsed, OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("TryApplyRuleString: не удалось разобрать '%s' - %s"), *InRuleString, *OutError);
		return false;
	}

	// Присваиваем по имени поля, не позиционно - порядок полей в строке
	// (Survival, затем Birth) не совпадает с порядком объявления
	// BirthCounts/SurvivalCounts здесь.
	SurvivalCounts = Parsed.SurvivalCounts;
	BirthCounts = Parsed.BirthCounts;
	States = Parsed.States;
	Neighborhood = Parsed.Neighborhood;

	// Строку, пришедшую параметром (из HUD или из пресета), кладём в
	// UPROPERTY - иначе Details panel показывал бы прежнее правило, хотя
	// считается уже по новому. При вызове из ApplyRuleString() это
	// самоприсваивание, безвредное.
	RuleString = InRuleString;

	OutError.Reset();

	UE_LOG(LogTemp, Log, TEXT("TryApplyRuleString: '%s' -> BirthCounts=%d знач., SurvivalCounts=%d знач., States=%d, Neighborhood=%s"),
		*InRuleString, BirthCounts.Num(), SurvivalCounts.Num(), States,
		Neighborhood == ENeighborhood::Moore ? TEXT("Moore") : TEXT("VonNeumann"));

	return true;
}

FString AAutomataOrchestrator::GetActiveRuleString() const
{
	return RuleStringParser::FormatRuleString(SurvivalCounts, BirthCounts, States, Neighborhood);
}

TArray<FRulePreset> AAutomataOrchestrator::GetRulePresets() const
{
	return RulePresets::GetAll();
}

void AAutomataOrchestrator::ApplyRulePreset(int32 PresetIndex, bool bApplySpawnSettings)
{
	const TArray<FRulePreset>& Presets = RulePresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyRulePreset: индекс %d вне диапазона (пресетов: %d)"), PresetIndex, Presets.Num());
		return;
	}

	const FRulePreset& Preset = Presets[PresetIndex];

	FString Error;
	if (!TryApplyRuleString(Preset.RuleString, Error))
	{
		// Строки в таблице пресетов константны, так что сюда можно попасть
		// только если таблица разъехалась с парсером - это ошибка кода, а не
		// пользовательский ввод, поэтому Error, а не Warning.
		UE_LOG(LogTemp, Error, TEXT("ApplyRulePreset: пресет '%s' содержит неразбираемое правило '%s' - %s"),
			*Preset.Name, *Preset.RuleString, *Error);
		return;
	}

	if (bApplySpawnSettings)
	{
		SpawnRadius = Preset.SpawnRadius;
		Amount = Preset.Amount;
	}

	UE_LOG(LogTemp, Log, TEXT("ApplyRulePreset: '%s' (%s)%s"),
		*Preset.Name, *Preset.RuleString,
		bApplySpawnSettings
			? *FString::Printf(TEXT(", SpawnRadius=%d, Amount=%d"), SpawnRadius, Amount)
			: TEXT(""));
}

void AAutomataOrchestrator::SpawnRuleVerificationPattern()
{
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnRuleVerificationPattern: фоновый шаг StepAsync() ещё считается - подождите его завершения"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnRuleVerificationPattern: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnRuleVerificationPattern: активный CellsMesh-компонент отсутствует"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnRuleVerificationPattern: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	ClearBakedMesh();
	ClearGhostShape();

	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	ResetGenerationCounter();
	SelectedCells.Reset();

	// Все три паттерна - классические 2D-фигуры (см. doc-comment в заголовке),
	// целиком в плоскости Z=0, разнесены минимум на 6 пустых клеток друг от
	// друга (радиус влияния Moore - 1 клетка, этого с большим запасом
	// достаточно, чтобы они не мешали друг другу).
	TArray<FIntVector> Cells = {
		// Блок (неподвижка) - 2x2, должен остаться абсолютно неизменным на
		// любом числе шагов.
		{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},

		// Мигалка (осциллятор периода 2) - горизонтальная тройка, каждый шаг
		// переключается в вертикальную и обратно.
		{10, 0, 0}, {11, 0, 0}, {12, 0, 0},

		// Планер - классическая ориентация, за 4 поколения сдвигается на
		// (+1, +1), сохраняя форму.
		{21, 0, 0}, {22, 1, 0}, {20, 2, 0}, {21, 2, 0}, {22, 2, 0},
	};

	for (const FIntVector& Cell : Cells)
	{
		Grid->SetAlive(Cell, true);
	}

	// Та же "точка возврата", что и у GenerateRandom()/StartFromSelection() -
	// R воспроизводит ровно этот паттерн заново, не случайную генерацию.
	InitialStateCells = Cells;

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("SpawnRuleVerificationPattern: посажены блок/мигалка/планер (%d клеток) - выставьте BirthCounts=[3], SurvivalCounts=[2,3], Neighborhood=Moore и шагайте F, сверяя с классическим поведением 2D Game of Life"),
		Grid->Num());
}

void AAutomataOrchestrator::AdjustSpeed(float Delta)
{
	// Верхняя граница здесь выше, чем UIMax в UPROPERTY-метаданных Speed
	// (10.0) - тот UIMax только ограничивает слайдер в Details panel, не сам
	// ClampMax, так что хоткеям +/- можно позволить разогнать Speed дальше.
	Speed = FMath::Clamp(Speed + Delta, 0.1f, 100.0f);
	UE_LOG(LogTemp, Log, TEXT("AdjustSpeed: Speed = %.2f"), Speed);
}

void AAutomataOrchestrator::SetSpeed(float NewSpeed)
{
	// Тот же кламп, что в AdjustSpeed() (см. комментарий там про то, почему
	// верхняя граница шире UIMax свойства).
	Speed = FMath::Clamp(NewSpeed, 0.1f, 100.0f);
	UE_LOG(LogTemp, Log, TEXT("SetSpeed: Speed = %.2f"), Speed);
}

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

void AAutomataOrchestrator::ShowStatusMessage(int32 Key, const FString& Message) const
{
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(Key, 3.0f, FColor::Cyan, Message);
	}
}

bool AAutomataOrchestrator::ShouldRefreshViewSlice() const
{
	if (!bEnableViewSlice)
	{
		return false;
	}

	FVector Location;
	FVector Forward;
	if (!GetCameraView(Location, Forward))
	{
		return false;
	}

	if (!bHasViewSliceCameraState)
	{
		return true;
	}

	if (FVector::Dist(Location, LastViewSliceCameraLocation) > ViewSliceCameraMoveThreshold)
	{
		return true;
	}

	// Поворот сравнивается через скалярное произведение направлений, а не
	// через разницу углов Эйлера: у последних есть разрывы (переход через
	// 360, gimbal-эффекты у pitch), из-за которых порог срабатывал бы то
	// впустую, то не срабатывал вовсе.
	const float CosThreshold = FMath::Cos(FMath::DegreesToRadians(ViewSliceRotationThreshold));
	return FVector::DotProduct(Forward, LastViewSliceCameraForward) < CosThreshold;
}

void AAutomataOrchestrator::SetViewSliceEnabled(bool bEnabled)
{
	bEnableViewSlice = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("SetViewSliceEnabled: срез вдоль взгляда %s"), bEnableViewSlice ? TEXT("включён") : TEXT("выключен"));
	ShowStatusMessage(StatusKey_ViewSlice, bEnableViewSlice
		? FString::Printf(TEXT("[J] Срез вдоль взгляда ВКЛ  -  середина %.0f, толщина %.0f  ([ ] двигать, Shift+[ ] толщина)"), ViewSliceDistance, ViewSliceThickness)
		: FString(TEXT("[J] Срез вдоль взгляда ВЫКЛ")));

	// Тик нужен самому срезу, а не только симуляции: он следит за камерой
	// (см. ShouldRefreshViewSlice() в Tick()), а разглядывают структуру как
	// раз на паузе, когда актор иначе не тикал бы вообще
	// (bStartWithTickEnabled = false, включает только Start()). Выключая срез,
	// возвращаем тик тому, кто в нём ещё нуждается.
	SetActorTickEnabled(bEnableViewSlice || bSimulationRunning || bFastStepActive);

	// При включении сбрасываем запомненное положение камеры - иначе первый
	// Tick() сравнил бы с состоянием от прошлого включения и мог решить, что
	// перестраивать не нужно.
	bHasViewSliceCameraState = false;
	// Немедленно, а не со следующим поколением - на паузе следующего может и
	// не быть (та же причина, что у SetRenderCullVolumeEnabled()).
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::SetAgeFilter(int32 NewAgeFilter, bool bIncludeOlder)
{
	AgeFilterValues.Reset();
	if (NewAgeFilter >= 0)
	{
		AgeFilterValues.Add(FMath::Clamp(NewAgeFilter, 0, 255));
	}

	// При снятом фильтре флаг бессмыслен, и оставленный включённым он путал бы
	// и Details-панель, и следующее нажатие цифры.
	bAgeFilterIncludesOlder = AgeFilterValues.Num() > 0 && bIncludeOlder;

	ApplyAgeFilterChange();
}

void AAutomataOrchestrator::ToggleAgeFilterValue(int32 Age, bool bIncludeOlder)
{
	if (Age < 0 || Age > 255)
	{
		UE_LOG(LogTemp, Warning, TEXT("ToggleAgeFilterValue: возраст %d вне диапазона 0..255"), Age);
		return;
	}

	if (AgeFilterValues.Remove(Age) > 0)
	{
		// Флаг "и всё, что старше" принадлежит той цифре, которая его подняла,
		// и уходит вместе с ней - иначе Shift+9 убрал бы девятку, но оставил
		// висеть весь хвост рампы, что выглядит как "не сработало".
		if (bIncludeOlder)
		{
			bAgeFilterIncludesOlder = false;
		}
	}
	else
	{
		AgeFilterValues.Add(Age);
		if (bIncludeOlder)
		{
			bAgeFilterIncludesOlder = true;
		}
	}

	// Убрали последний выбранный возраст - фильтра больше нет, а значит нет и
	// хвоста: пустой список и "показывать все" - одно состояние, а не два.
	if (AgeFilterValues.Num() == 0)
	{
		bAgeFilterIncludesOlder = false;
	}

	ApplyAgeFilterChange();
}

void AAutomataOrchestrator::ApplyAgeFilterChange()
{
	// Канонизация нужна не только после ручной правки списка в Details-панели:
	// от неё зависит и порядок в сообщении на экране, и то, что повторное
	// Shift+цифра действительно находит уже добавленный возраст.
	for (int32 Index = AgeFilterValues.Num() - 1; Index >= 0; --Index)
	{
		const int32 Value = AgeFilterValues[Index];
		if (Value < 0 || Value > 255)
		{
			AgeFilterValues.RemoveAt(Index);
		}
	}

	AgeFilterValues.Sort();

	for (int32 Index = AgeFilterValues.Num() - 1; Index > 0; --Index)
	{
		if (AgeFilterValues[Index] == AgeFilterValues[Index - 1])
		{
			AgeFilterValues.RemoveAt(Index);
		}
	}

	const FString Description = DescribeAgeFilter();

	// Строки собираются заранее, а не тернарником внутри Printf(): формат-строка
	// проверяется на этапе компиляции (consteval TCheckedFormatString) и обязана
	// быть литералом, а не выбранным во время исполнения указателем.
	FString LogText(TEXT("фильтр снят, показываются все клетки"));
	FString StatusText(TEXT("Фильтр по возрасту снят"));
	if (AgeFilterValues.Num() > 0)
	{
		LogText = FString::Printf(TEXT("показываются только клетки: %s"), *Description);
		StatusText = FString::Printf(TEXT("Возраст: %s  (Shift+цифра - добавить/убрать, та же цифра - показать все)"), *Description);
	}

	UE_LOG(LogTemp, Log, TEXT("SetAgeFilter: %s"), *LogText);
	ShowStatusMessage(StatusKey_AgeFilter, StatusText);
	RefreshRenderCullVolume();
}

FString AAutomataOrchestrator::DescribeAgeFilter() const
{
	if (AgeFilterValues.Num() == 0)
	{
		return TEXT("все возрасты");
	}

	FString Result;
	for (int32 Index = 0; Index < AgeFilterValues.Num(); ++Index)
	{
		if (Index > 0)
		{
			Result += TEXT(", ");
		}
		Result += FString::FromInt(AgeFilterValues[Index]);
	}

	// Хвост относится к самому старому из выбранных - только он и может быть
	// открытым сверху.
	if (bAgeFilterIncludesOlder)
	{
		Result += TEXT(" и старше");
	}

	return Result;
}

bool AAutomataOrchestrator::BuildAgeFilterMask(TArray<bool>& OutMask) const
{
	if (AgeFilterValues.Num() == 0)
	{
		return false;
	}

	OutMask.Init(false, 256);

	int32 MaxSelected = 0;
	for (const int32 Value : AgeFilterValues)
	{
		if (Value < 0 || Value > 255)
		{
			continue;
		}

		OutMask[Value] = true;
		MaxSelected = FMath::Max(MaxSelected, Value);
	}

	// "И всё, что старше" открывает диапазон вверх от самого старого из
	// выбранных: цифр десять, а возрастов 256, и без этого хвост рампы не
	// показывался бы ни под какой цифрой.
	if (bAgeFilterIncludesOlder)
	{
		for (int32 Age = MaxSelected; Age < 256; ++Age)
		{
			OutMask[Age] = true;
		}
	}

	return true;
}

void AAutomataOrchestrator::AdjustViewSliceDistance(float Delta)
{
	ViewSliceDistance = FMath::Max(ViewSliceDistance + Delta, 0.0f);
	UE_LOG(LogTemp, Log, TEXT("AdjustViewSliceDistance: середина среза на %.0f от камеры"), ViewSliceDistance);
	// Сообщение показывается и когда срез выключен: иначе нажатие [ / ] при
	// выключенном срезе выглядело бы как "клавиша не работает", хотя значение
	// исправно меняется и подействует при включении.
	ShowStatusMessage(StatusKey_ViewSlice, FString::Printf(TEXT("[%s] Срез: середина %.0f, толщина %.0f%s"),
		Delta < 0.0f ? TEXT("[") : TEXT("]"), ViewSliceDistance, ViewSliceThickness,
		bEnableViewSlice ? TEXT("") : TEXT("   (срез ВЫКЛЮЧЕН - включить J)")));
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::AdjustViewSliceThickness(float Delta)
{
	ViewSliceThickness = FMath::Max(ViewSliceThickness + Delta, 1.0f);
	UE_LOG(LogTemp, Log, TEXT("AdjustViewSliceThickness: толщина среза %.0f"), ViewSliceThickness);
	// См. одноимённый комментарий в AdjustViewSliceDistance().
	ShowStatusMessage(StatusKey_ViewSlice, FString::Printf(TEXT("[Shift+%s] Срез: середина %.0f, толщина %.0f%s"),
		Delta < 0.0f ? TEXT("[") : TEXT("]"), ViewSliceDistance, ViewSliceThickness,
		bEnableViewSlice ? TEXT("") : TEXT("   (срез ВЫКЛЮЧЕН - включить J)")));
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::AdjustStepsPerRender(int32 Delta)
{
	SetStepsPerRender(StepsPerRender + Delta);
}

void AAutomataOrchestrator::SetStepsPerRender(int32 NewStepsPerRender)
{
	StepsPerRender = FMath::Clamp(NewStepsPerRender, 1, MaxStepsPerRender);
	UE_LOG(LogTemp, Log, TEXT("SetStepsPerRender: StepsPerRender = %d"), StepsPerRender);
}

void AAutomataOrchestrator::ScaleStepsPerRender(bool bDouble)
{
	// Не умножение на два, а переход к следующей/предыдущей СТЕПЕНИ ДВОЙКИ:
	// если текущее значение степенью двойки не является (например 254,
	// набранное с клавиши), удвоение оставило бы его таким же неровным - 508.
	// Так же одно нажатие всегда приводит на 2^k, откуда дальше можно ходить
	// по степеням точно.
	const int32 Current = FMath::Clamp(StepsPerRender, 1, MaxStepsPerRender);

	int32 Next;
	if (bDouble)
	{
		Next = 1;
		while (Next <= Current && Next < MaxStepsPerRender)
		{
			Next <<= 1;
		}
	}
	else
	{
		Next = MaxStepsPerRender;
		while (Next >= Current && Next > 1)
		{
			Next >>= 1;
		}
	}

	SetStepsPerRender(Next);
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

UMainHudWidget* AAutomataOrchestrator::GetHudWidget() const
{
	if (!UiController)
	{
		return nullptr;
	}

	// Cast, а не static_cast: HUDWidgetClass задаётся в Details-панели и
	// формально может оказаться любым UUserWidget - тогда честнее вернуть
	// nullptr, чем притвориться, что это наш виджет.
	return Cast<UMainHudWidget>(UiController->GetWidget());
}

void AAutomataOrchestrator::ToggleHudInfoPanel()
{
	UMainHudWidget* Widget = GetHudWidget();
	if (!Widget)
	{
		UE_LOG(LogTemp, Warning, TEXT("ToggleHudInfoPanel: HUD-виджет не создан или не унаследован от UMainHudWidget"));
		return;
	}

	// Что именно прячется, решает граф в Blueprint - см. doc-comment события.
	Widget->OnToggleInfoPanel();
}

void AAutomataOrchestrator::ToggleHud()
{
	if (!UiController)
	{
		UE_LOG(LogTemp, Warning, TEXT("ToggleHud: HUD не инициализирован (InitializeHUD() вызывается из BeginPlay)"));
		return;
	}

	UiController->ToggleHUD();
}

bool AAutomataOrchestrator::IsHudVisible() const
{
	return UiController && UiController->IsHUDVisible();
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

void AAutomataOrchestrator::ClearInactiveCellsMeshComponent()
{
	UInstancedStaticMeshComponent* InactiveComponent =
		(CellMeshComponentType == ECellMeshComponentType::HierarchicalInstanced)
			? static_cast<UInstancedStaticMeshComponent*>(CellsMeshFlat)
			: static_cast<UInstancedStaticMeshComponent*>(CellsMeshHierarchical);

	if (InactiveComponent && InactiveComponent->GetInstanceCount() > 0)
	{
		InactiveComponent->ClearInstances();
	}
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

void AAutomataOrchestrator::EnsureCellsRenderer()
{
	UInstancedStaticMeshComponent* Target = GetActiveCellsMeshComponent();
	if (!Target)
	{
		return;
	}

	// Одно условие на три случая: рендерера ещё нет (первый вызов либо
	// обнуление после реинстансинга Live Coding - сами компоненты default
	// subobject'ы и переживают его, а TUniquePtr нет), либо он обёрнут вокруг
	// другого компонента (поменяли CellMeshComponentType).
	if (CellsRenderer && CellsRenderer->GetComponent() == Target)
	{
		return;
	}

	// Перепривязка: прежний компонент обязан остаться пустым, иначе его
	// инстансы висят внахлёст с новыми.
	if (CellsRenderer)
	{
		if (UInstancedStaticMeshComponent* PreviousComponent = CellsRenderer->GetComponent())
		{
			PreviousComponent->ClearInstances();
		}
	}

	CellsRenderer = MakeUnique<FInstancedMeshCellGridRenderer>(Target);
}

FLinearColor AAutomataOrchestrator::SampleColorRamp(const TArray<FLinearColor>& Keys, float T)
{
	if (Keys.Num() == 0)
	{
		// Белый, а не отказ рисовать: пустая рампа - это нормальное
		// промежуточное состояние настройки, и белый цвет означает "как
		// выглядит сам материал" (см. doc-comment AgeColors).
		return FLinearColor::White;
	}
	if (Keys.Num() == 1)
	{
		return Keys[0];
	}

	const float Position = FMath::Clamp(T, 0.0f, 1.0f) * float(Keys.Num() - 1);
	const int32 LowIndex = FMath::Clamp(FMath::FloorToInt(Position), 0, Keys.Num() - 1);
	const int32 HighIndex = FMath::Min(LowIndex + 1, Keys.Num() - 1);
	return FMath::Lerp(Keys[LowIndex], Keys[HighIndex], Position - float(LowIndex));
}

void AAutomataOrchestrator::BuildAgeColorLut(TArray<FColor>& OutLut, bool bSRGB) const
{
	OutLut.SetNumUninitialized(256);
	const float MaxAge = float(FMath::Max(1, AgeColorMaxAge));
	for (int32 Age = 0; Age < 256; ++Age)
	{
		// bSRGB=false обязательно: PerInstanceCustomData это сырой float,
		// материал никакого sRGB-декода не делает - гамма-кодирование здесь
		// тихо испортило бы всю рампу (см. FCellRenderInstance).
		OutLut[Age] = SampleColorRamp(AgeColors, float(Age) / MaxAge).ToFColor(bSRGB);
	}
}

void AAutomataOrchestrator::BuildDecayColorLut(TArray<FColor>& OutLut, bool bSRGB) const
{
	OutLut.SetNumUninitialized(256);

	// Пустой DecayColors - берём возрастную рампу, т.е. "как было до появления
	// отдельной шкалы угасания" (см. doc-comment DecayColors).
	const TArray<FLinearColor>& Ramp = (DecayColors.Num() > 0) ? DecayColors : AgeColors;

	// Стадии угасания - это [2 .. States-1]: 2 "только начала гаснуть",
	// States-1 "последняя стадия перед смертью". Итого States-2 стадий, а
	// значит States-3 интервалов между ними. При States == 3 стадия ровно
	// одна - знаменатель зажимаем в 1, T выходит 0, берётся первый ключ.
	const int32 Denominator = FMath::Max(1, States - 3);
	for (int32 State = 0; State < 256; ++State)
	{
		const float T = float(FMath::Clamp(State - 2, 0, Denominator)) / float(Denominator);
		OutLut[State] = SampleColorRamp(Ramp, T).ToFColor(bSRGB);
	}
}

void AAutomataOrchestrator::EnsureSelectionMeshComponent()
{
	if (SelectionMeshComponent && !SelectionRenderer)
	{
		// Пережил реинстансинг Live Coding (UPROPERTY), а SelectionRenderer
		// (обычный член) - нет; EnsureCellsRenderer() ловит ровно тот же
		// сценарий для CellsRenderer.
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

	if (SelectedCells.Num() > 0 && !CellMaterial)
	{
		// Иначе подсветка молча не рисуется, и выглядит это как "выделение
		// не работает" - уже кусало при настройке.
		UE_LOG(LogTemp, Warning, TEXT("RenderSelectionOverlay: CellMaterial не назначен - подсветка выделения не будет видна, назначьте материал клеток в Details panel"));
	}

	if (!Grid || SelectedCells.Num() == 0 || !CellMaterial)
	{
		SelectionMeshComponent->ClearInstances();
		return;
	}

	// Отфильтровываем до реально живых - на случай, если SelectedCells
	// вызвали до какого-то не прошедшего через инвалидацию изменения Grid
	// (сегодня такого пути нет, но проверка дешёвая, а рассинхрон иначе тихий).
	const FColor HighlightColor = SelectionColor.ToFColor(/*bSRGB=*/false);
	TArray<FCellRenderInstance> SelectionInstances;
	SelectionInstances.Reserve(SelectedCells.Num());
	for (const FIntVector& Cell : SelectedCells)
	{
		if (Grid->IsAlive(Cell))
		{
			SelectionInstances.Add({ FVector3f(Grid->GridToWorld(Cell)), HighlightColor });
		}
	}

	if (SelectionInstances.Num() == 0)
	{
		SelectionMeshComponent->ClearInstances();
		return;
	}

	// Тот же материал, что и у обычных клеток - отличается только цветом в
	// per-instance custom data (см. doc-comment SelectionColor).
	SelectionRenderer->SetMesh(CellMesh);
	SelectionRenderer->SetMaterial(CellMaterial);
	// Чуть крупнее обычного кубика - иначе поверхности совпадают и мерцают
	// (z-fighting), см. doc-comment SelectionScaleMultiplier.
	SelectionRenderer->SetScaleMultiplier(SelectionScaleMultiplier);

	// Всегда одним снимком - выделение всегда маленькое, чанкинг не нужен
	// даже во время непрерывного Play.
	SelectionRenderer->Render(*Grid, MoveTemp(SelectionInstances));
}

void AAutomataOrchestrator::SelectCellsInScreenRect(const FMatrix& ViewProjectionMatrix, const FVector2D& ViewportSize, const FVector2D& RectMin, const FVector2D& RectMax, ESelectionCombineMode CombineMode)
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellsInScreenRect: сетка не инициализирована"));
		return;
	}

	// Куб отсечения (см. bEnableRenderCullVolume) прячет клетки снаружи себя
	// от рендера (BuildCellRenderData() ограничивает по тем же границам) -
	// выделение обязано ловить ровно то же подмножество, иначе марки видят
	// клетки, которых физически нет на экране.
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	TArray<FIntVector> RectCells;
	if (CullVolume)
	{
		TArray<FIntVector> VisibleCells;
		Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), VisibleCells);
		FFilteredCellGridView VisibleView(*Grid, MoveTemp(VisibleCells));
		RectCells = CellSelection::SelectCellsInScreenRect(VisibleView, ViewProjectionMatrix, ViewportSize, RectMin, RectMax);
	}
	else
	{
		RectCells = CellSelection::SelectCellsInScreenRect(*Grid, ViewProjectionMatrix, ViewportSize, RectMin, RectMax);
	}
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
	// дальше живых клеток гарантированно нет, шагать бессмысленно. Считаем
	// от ПОЛНОГО набора живых клеток (не от отфильтрованного по кубу ниже) -
	// это только верхняя граница длины луча, а не источник кандидатов, так
	// что запас безопасен и в режиме с активным кубом.
	FVector BoundsCenter = FVector::ZeroVector;
	float BoundsRadius = 0.0f;
	if (!ComputeAliveCellsBounds(BoundsCenter, BoundsRadius))
	{
		UE_LOG(LogTemp, Log, TEXT("SelectCellUnderCursor: живых клеток нет - выделять нечего"));
		return;
	}
	const double MaxDistance = FVector::Distance(RayOrigin, BoundsCenter) + BoundsRadius + Grid->GetCellSize();

	// Тот же принцип, что у SelectCellsInScreenRect() выше - если куб
	// активен, клик должен "видеть" ровно то подмножество клеток, которое
	// реально нарисовано, а не всю сетку насквозь. FFilteredCellGridView::
	// IsAlive() (единственное, что использует DDA-обход PickCellAlongRay())
	// согласован с отфильтрованным набором - см. её doc-comment.
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	TUniquePtr<FFilteredCellGridView> VisibleView;
	const FCellGrid* PickGrid = Grid.Get();
	if (CullVolume)
	{
		TArray<FIntVector> VisibleCells;
		Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), VisibleCells);
		VisibleView = MakeUnique<FFilteredCellGridView>(*Grid, MoveTemp(VisibleCells));
		PickGrid = VisibleView.Get();
	}

	TArray<FIntVector> PickedCells;
	FIntVector PickedCell;
	if (CellSelection::PickCellAlongRay(*PickGrid, RayOrigin, RayDirection, MaxDistance, PickedCell))
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

bool AAutomataOrchestrator::MoveCullVolumeToChunkUnderCursor(const FVector& RayOrigin, const FVector& RayDirection)
{
	if (!Grid)
	{
		return false;
	}

	const float ChunkWorldSize = Grid->GetChunkWorldSize();
	if (ChunkWorldSize <= 0.0f)
	{
		// Сетка без чанков - выбирать нечего (см. FCellGrid::GetChunkWorldSize()).
		return false;
	}

	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	if (!CullVolume)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeToChunkUnderCursor: на уровне нет ARenderCullVolume - разместите его сначала"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("На уровне нет ARenderCullVolume - разместите его"));
		return false;
	}

	FVector BoundsCenter = FVector::ZeroVector;
	float BoundsRadius = 0.0f;
	if (!ComputeAliveCellsBounds(BoundsCenter, BoundsRadius))
	{
		return false;
	}
	const double MaxDistance = FVector::Distance(RayOrigin, BoundsCenter) + BoundsRadius + ChunkWorldSize;

	// Тот же DDA, что ищет клетку под курсором - он принимает абстрактный
	// FCellGrid и не знает, клетки в нём или чанки. FChunkGridView - это и
	// есть сетка из чанков (её же строит гост-силуэт), только здесь она
	// нужна с НАСТОЯЩИМ IsAlive(): иначе луч вернул бы первый задетый чанк,
	// включая пустые (см. bBuildOccupancySet в её конструкторе).
	TArray<FIntVector> OccupiedChunks;
	Grid->GetOccupiedChunkCoords(OccupiedChunks);
	if (OccupiedChunks.Num() == 0)
	{
		return false;
	}

	const FChunkGridView ChunkView(ChunkWorldSize, Grid->GetCellSize(), MoveTemp(OccupiedChunks), /*bBuildOccupancySet=*/true);

	FIntVector PickedChunk;
	if (!CellSelection::PickCellAlongRay(ChunkView, RayOrigin, RayDirection, MaxDistance, PickedChunk))
	{
		ShowStatusMessage(StatusKey_CullVolume, TEXT("Клик мимо - под курсором нет занятых чанков"));
		return true;
	}

	// GridToWorld() у этой вьюхи специально возвращает ЦЕНТР чанка, а не его
	// угол (см. её doc-comment) - то есть ровно то, во что надо поставить куб.
	const FVector ChunkCenter = ChunkView.GridToWorld(PickedChunk);
	CullVolume->SetActorLocation(ChunkCenter);

	UE_LOG(LogTemp, Log, TEXT("MoveCullVolumeToChunkUnderCursor: куб отсечения переставлен на чанк %s (мир: %s)"),
		*PickedChunk.ToString(), *ChunkCenter.ToString());
	ShowStatusMessage(StatusKey_CullVolume, FString::Printf(
		TEXT("Куб отсечения на чанк %s.  H - убрать силуэт, C - включить отсечение"), *PickedChunk.ToString()));

	// SetActorLocation() программно не поднимает PostEditMove() - перерисовываем
	// сами, как в MoveCullVolumeToSelection().
	RefreshRenderCullVolume();
	return true;
}

void AAutomataOrchestrator::SelectCellsInCullVolume(ESelectionCombineMode CombineMode)
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellsInCullVolume: сетка не инициализирована"));
		return;
	}

	// EnsureRenderCullVolume() напрямую, не через bEnableRenderCullVolume -
	// куб как пространственная область существует независимо от того,
	// используется ли он сейчас для отсечения рендера (см. doc-comment в
	// заголовке).
	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	if (!CullVolume)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellsInCullVolume: на уровне нет ARenderCullVolume - разместите его сначала"));
		return;
	}

	TArray<FIntVector> CellsInVolume;
	Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), CellsInVolume);
	CombineWithSelection(MoveTemp(CellsInVolume), CombineMode);

	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("SelectCellsInCullVolume: выделено %d клеток (режим: %s)"),
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

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("DeleteSelectedCells: CellMaterial не назначен - назначьте материал клеток в Details panel"));
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

void AAutomataOrchestrator::ClearAllCellInstances()
{
	// Полный обход ВСЕХ реально прикреплённых к актору
	// UInstancedStaticMeshComponent, а не только тех, что перечислены в
	// CellsMeshFlat/CellsMeshHierarchical/SelectionMeshComponent - защита от
	// осиротевших компонентов: лишние компоненты остаются
	// physически прикреплены и видимы, продолжая рисовать свои старые
	// инстансы поверх честно посчитанных - visуально выглядит как
	// наложение/мерцание двух состояний, хотя логическое состояние
	// симуляции (Grid) при этом только одно. Обнаруженный на практике
	// случай (ещё во времена пула возрастных компонентов): материалов было 3,
	// а на акторе висело 8 InstancedStaticMeshComponent - 5 лишних,
	// ClearInstances() по одному только легитимному набору их не касался.
	//
	// После перехода на per-instance цвет этот же механизм заодно подчищает
	// сам бывший пул: легитимный набор сократился до трёх компонентов, и все
	// рантайм-созданные возрастные компоненты стали здесь сиротами.
	TArray<UInstancedStaticMeshComponent*> AllInstancedComponents;
	GetComponents<UInstancedStaticMeshComponent>(AllInstancedComponents);

	TSet<UInstancedStaticMeshComponent*> KeepSet;
	if (CellsMeshFlat)
	{
		KeepSet.Add(CellsMeshFlat);
	}
	if (CellsMeshHierarchical)
	{
		KeepSet.Add(CellsMeshHierarchical);
	}
	if (SelectionMeshComponent)
	{
		KeepSet.Add(SelectionMeshComponent);
	}

	for (UInstancedStaticMeshComponent* Comp : AllInstancedComponents)
	{
		if (!Comp)
		{
			continue;
		}

		if (KeepSet.Contains(Comp))
		{
			Comp->ClearInstances();
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ClearAllCellInstances: обнаружен и уничтожен осиротевший компонент %s (не входит в текущий легитимный набор)"), *Comp->GetName());
			Comp->DestroyComponent();
		}
	}
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

	// Куб отсечения активен (см. GetActiveCullVolume()) - оставляем только
	// чанки СНАРУЖИ куба, внутри уже рисует обычный детальный путь
	// (BuildCellRenderData()), силуэт здесь чистое дополнение.
	// Иначе (куб выключен хоткеем C, спрятан Ctrl+C, либо его вообще нет) -
	// границы отсечения нет, "снаружи" значит "везде": силуэт покрывает всю
	// сетку целиком и заменяет детальный рендер (см.
	// ShouldGhostShapeReplaceDetailedRender(), RenderGridImmediate()/
	// RenderCurrentGrid()) - именно этот режим и даёт выигрыш в скорости
	// при большом числе клеток без активного куба.
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	TArray<FIntVector> ChunksToGhost;
	if (CullVolume)
	{
		// Чанки на границе (частично внутри/снаружи) сознательно остаются
		// "внутри" (не отбрасываются) - минимальное дублирование на границе
		// дешевле точной обрезки.
		const FBox CullBounds = CullVolume->GetWorldBounds();
		ChunksToGhost.Reserve(OccupiedChunks.Num());
		for (const FIntVector& ChunkCoord : OccupiedChunks)
		{
			const FVector ChunkOrigin = FVector(ChunkCoord) * ChunkWorldSize;
			const FBox ChunkBounds(ChunkOrigin, ChunkOrigin + FVector(ChunkWorldSize));
			if (!CullBounds.Intersect(ChunkBounds))
			{
				ChunksToGhost.Add(ChunkCoord);
			}
		}
	}
	else
	{
		ChunksToGhost = OccupiedChunks;
	}

	if (ChunksToGhost.Num() == 0)
	{
		ClearGhostShape();
		return;
	}

	UMaterialInterface* MeshMaterial = GhostShapeMaterial;
	if (!MeshMaterial)
	{
		// Фолбэка больше нет: раньше подставлялся AgeMaterials[0], но
		// подставить сюда CellMaterial нельзя - он берёт цвет из per-instance
		// custom data, которых у UProceduralMeshComponent нет, и силуэт вышел
		// бы ЧЁРНЫМ, молча. Серый дефолт движка плюс эта строчка честнее.
		UE_LOG(LogTemp, Warning, TEXT("RefreshGhostShape: GhostShapeMaterial не назначен - силуэт будет нарисован дефолтным материалом движка"));
	}

	const double BuildStartSeconds = FPlatformTime::Seconds();
	FChunkGridView ChunkView(ChunkWorldSize, Grid->GetCellSize(), ChunksToGhost);
	CellMeshBuilder::FCellMeshData MeshData = CellMeshBuilder::BuildFromCells(ChunkView, ChunksToGhost);
	const double BuildSeconds = FPlatformTime::Seconds() - BuildStartSeconds;

	EnsureGhostMeshComponent();
	GhostMeshComponent->ClearAllMeshSections();
	GhostMeshComponent->CreateMeshSection_LinearColor(0, MeshData.Vertices, MeshData.Triangles, MeshData.Normals, MeshData.UVs,
		TArray<FLinearColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision=*/false);
	if (MeshMaterial)
	{
		GhostMeshComponent->SetMaterial(0, MeshMaterial);
	}

	UE_LOG(LogTemp, Log, TEXT("RefreshGhostShape: %d/%d чанков %s -> %d вершин / %d треугольников (%.2f мс)"),
		ChunksToGhost.Num(), OccupiedChunks.Num(), CullVolume ? TEXT("снаружи куба") : TEXT("(весь грид, куба нет)"),
		MeshData.Vertices.Num(), MeshData.Triangles.Num() / 3, BuildSeconds * 1000.0);
}

bool AAutomataOrchestrator::ShouldGhostShapeReplaceDetailedRender()
{
	if (!bEnableGhostShape)
	{
		return false;
	}

	// Нет активной границы отсечения (куб выключен, спрятан, либо на уровне
	// его вообще нет - см. GetActiveCullVolume()) - RefreshGhostShape() в
	// этом случае строит силуэт по ВСЕМ занятым чанкам (см. её doc-comment),
	// т.е. он уже покрывает всю сетку целиком, и детальный поклеточный
	// рендер (BuildCellRenderData()+AddInstances по каждой живой клетке) здесь
	// был бы именно той дорогой работой, которую эта фича должна заменять
	// при большом числе клеток. Пока куб активен, детальный путь и так уже
	// дешёвый (Grid->GetAliveCellsInBounds() ограничивает его объёмом куба) -
	// там силуэт остаётся чистым дополнением снаружи, детальный путь не
	// трогаем.
	return GetActiveCullVolume() == nullptr;
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

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: CellMaterial не назначен - назначьте материал клеток в Details panel"));
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
		return;
	}

	if (bStepInProgress)
	{
		// Не просто отказываем - откладываем до момента, когда фоновый шаг
		// сам применит свой результат (ApplyStepResult()/завершение Next()),
		// оба проверяют этот флаг сразу после сброса bStepInProgress и сами
		// вызовут ResetToInitialState() ещё раз. Раньше здесь был только
		// warning-лог и return без взведения флага - нажатие R, совпавшее с
		// фоновым шагом, терялось молча, симуляция просто продолжала идти
		// дальше без сброса (см. doc-comment bResetToInitialStatePending).
		bResetToInitialStatePending = true;
		// Взаимоисключающ с отложенным рероллом: R после N означает "верни
		// исходный узор", а не "сначала перекати сид" (см. bNewSeedPending).
		bNewSeedPending = false;
		UE_LOG(LogTemp, Warning, TEXT("ResetToInitialState: фоновый шаг StepAsync() ещё считается - сброс отложен до его завершения"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("ResetToInitialState: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("ResetToInitialState: CellMaterial не назначен - назначьте материал клеток в Details panel"));
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
	Header.States = States;
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
	States = FMath::Max(2, Header.States);
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

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: CellMaterial не назначен - назначьте материал клеток в Details panel"));
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

bool AAutomataOrchestrator::ComputeSelectedCellsBounds(FVector& OutCenter, float& OutRadius) const
{
	if (!Grid || SelectedCells.Num() == 0)
	{
		return false;
	}

	// Только ещё живые - та же защитная фильтрация, что в
	// RenderSelectionOverlay()/StartFromSelection(): выделение переживает шаги
	// симуляции, и клетка под ним могла давно умереть.
	TArray<FIntVector> Cells;
	Cells.Reserve(SelectedCells.Num());
	for (const FIntVector& Cell : SelectedCells)
	{
		if (Grid->IsAlive(Cell))
		{
			Cells.Add(Cell);
		}
	}

	return ComputeCellsBounds(Cells, OutCenter, OutRadius);
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
	// Запас на полклетки - GridToWorld() даёт координаты центра клетки, а не
	// её края (тот же запас, что был у прежней AABB-версии).
	OutRadius = static_cast<float>(Radius) + Grid->GetCellSize() * 0.5f;

	return true;
}

TUniquePtr<FCellGrid> AAutomataOrchestrator::CreateGrid() const
{
	return MakeUnique<FDenseCellGrid>(CellSize, ChunkSize, States > 2);
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

bool AAutomataOrchestrator::CanGenerateNewState(const TCHAR* LogPrefix) const
{
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: фоновый шаг StepAsync() ещё считается - подождите его завершения"), LogPrefix);
		return false;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: CellMesh не задан - назначьте StaticMesh в Details panel"), LogPrefix);
		return false;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: активный CellsMesh-компонент отсутствует"), LogPrefix);
		return false;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: CellMaterial не назначен - назначьте материал клеток в Details panel"), LogPrefix);
		return false;
	}

	return true;
}

void AAutomataOrchestrator::RebuildGridFromCells(TArray<FIntVector>&& Cells)
{
	// Новый прогон убирает запечённый меш-снимок, если он был (см.
	// BakeCellsToMesh()) - иначе новые клетки рисовались бы сквозь него.
	ClearBakedMesh();
	ClearGhostShape();

	// Всегда строим сетку с нуля - так подхватывается актуальный CellSize,
	// если его поменяли в Details panel.
	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	// Новый прогон - новый отсчёт поколений для HUD (см. GenerationCount/
	// FHudStats/ResetGenerationCounter()).
	ResetGenerationCounter();
	// Новая сетка делает старое выделение бессмысленным (координаты уже не
	// про эту сетку) - см. doc-comment SelectedCells в заголовке.
	SelectedCells.Reset();

	// Заливка строго последовательная: FCellGrid::SetAlive() кеширует последний
	// чанк и не потокобезопасен. Возраст намеренно не трогаем - ровно как
	// делал GenerateRandom() до появления генераторов (свежие чанки и так
	// зануляют Ages), в отличие от StartFromSelection(), который зовёт
	// SetAge() явно.
	for (const FIntVector& Cell : Cells)
	{
		Grid->SetAlive(Cell, true);
	}

	// Освобождаем массив генератора ДО того, как соберём InitialStateCells:
	// иначе на миллионах клеток пик держал бы оба массива разом.
	Cells.Empty();

	// Свежесгенерированное состояние - тоже валидная "точка возврата" R и
	// то, что уйдёт в файл при Save (см. doc-comment InitialStateCells) -
	// ровно как после StartFromSelection()/LoadStateFromFile(). Берём
	// фактически осевшие в сетке клетки, а не то, что отдал генератор:
	// у случайного шара броски дают коллизии в одну и ту же клетку.
	Grid->GetAliveCells(InitialStateCells);

	RenderGridImmediate();
}

void AAutomataOrchestrator::GenerateRandom()
{
	if (!CanGenerateNewState(TEXT("GenerateRandom")))
	{
		return;
	}

	// Параметры ЛОКАЛЬНЫЕ, а не GenerationParams, и это принципиально: этот
	// путь - автостарт в BeginPlay(), хоткей N (NewSeed()) и запасная ветка
	// ResetToInitialState(). Читай он панель генераторов, накрученный там
	// каркас тихо переопределил бы смысл всех трёх.
	FStateGeneratorParams RandomBallParams;
	RandomBallParams.Type = EStateGeneratorType::RandomBall;
	RandomBallParams.Radius = SpawnRadius;
	RandomBallParams.Amount = Amount;

	TArray<FIntVector> Cells;
	StateGenerators::FGenerateStats Stats;
	FString Error;

	// Без потолка: исторически этот путь ничего не ограничивал, и добавлять
	// ему отказ значило бы менять наблюдаемое поведение.
	if (!StateGenerators::Generate(RandomBallParams, Seed, MAX_int64, Cells, Stats, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandom: %s"), *Error);
		return;
	}

	RebuildGridFromCells(MoveTemp(Cells));

	UE_LOG(LogTemp, Log, TEXT("GenerateRandom: заспавнено %d клеток в радиусе %d (генерация: %.2f мс)"),
		Grid->Num(), SpawnRadius, Stats.Seconds * 1000.0);
}

void AAutomataOrchestrator::GenerateState()
{
	if (!CanGenerateNewState(TEXT("GenerateState")))
	{
		return;
	}

	const FString GeneratorName = StateGenerators::GetDisplayName(GenerationParams.Type);

	// Оценка ДО единого касания сетки: отказ обязан оставить текущее состояние
	// целым, а не стереть его и остановиться на полпути (та же идиома, что у
	// бюджета бейка - см. BakeCellsToMesh()).
	const int64 Estimate = StateGenerators::EstimateCellCount(GenerationParams);
	if (Estimate > MaxGeneratedCells)
	{
		const FString Message = FString::Printf(
			TEXT("Генератор '%s': ожидается %lld клеток при пределе %lld - уменьшите область или поднимите MaxGeneratedCells"),
			*GeneratorName, Estimate, MaxGeneratedCells);

		UE_LOG(LogTemp, Warning, TEXT("GenerateState: %s"), *Message);
		ShowStatusMessage(StatusKey_Generation, Message);
		return;
	}

	TArray<FIntVector> Cells;
	StateGenerators::FGenerateStats Stats;
	FString Error;

	if (!StateGenerators::Generate(GenerationParams, Seed, MaxGeneratedCells, Cells, Stats, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateState: %s"), *Error);
		ShowStatusMessage(StatusKey_Generation, FString::Printf(TEXT("Генерация не удалась: %s"), *Error));
		return;
	}

	// Гистограмму считаем ДО заливки, пока набор клеток ещё на руках.
	FString HistogramText;
	if (GenerationParams.bAnalyzeNeighborCounts)
	{
		StateGenerators::FNeighborHistogram Histogram;
		StateGenerators::AnalyzeNeighborCounts(Cells, Neighborhood, NeighborAnalysisSampleExtent, Histogram);
		HistogramText = StateGenerators::DescribeHistogram(Histogram);
	}

	RebuildGridFromCells(MoveTemp(Cells));

	UE_LOG(LogTemp, Log, TEXT("GenerateState: '%s' - клеток %d (перебрано %lld, генерация: %.2f мс)"),
		*GeneratorName, Grid->Num(), Stats.ScannedCells, Stats.Seconds * 1000.0);

	if (!HistogramText.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("GenerateState: соседи по %s - %s"),
			Neighborhood == ENeighborhood::VonNeumann ? TEXT("von Neumann") : TEXT("Moore"),
			*HistogramText);
	}

	ShowStatusMessage(StatusKey_Generation,
		FString::Printf(TEXT("Генератор: %s - %d клеток"), *GeneratorName, Grid->Num()));
}

void AAutomataOrchestrator::CycleStateGeneratorType()
{
	const int32 TypeCount = static_cast<int32>(EStateGeneratorType::SymmetricSeed) + 1;
	const int32 NextType = (static_cast<int32>(GenerationParams.Type) + 1) % TypeCount;
	GenerationParams.Type = static_cast<EStateGeneratorType>(NextType);

	const FString GeneratorName = StateGenerators::GetDisplayName(GenerationParams.Type);

	// Только переключаем тип, не генерируем: параметры нового типа почти всегда
	// хочется посмотреть и поправить до построения.
	UE_LOG(LogTemp, Log, TEXT("CycleStateGeneratorType: %s (оценка: %lld клеток)"),
		*GeneratorName, StateGenerators::EstimateCellCount(GenerationParams));

	ShowStatusMessage(StatusKey_Generation,
		FString::Printf(TEXT("Генератор: %s (~%lld клеток) - Y чтобы построить"),
			*GeneratorName, StateGenerators::EstimateCellCount(GenerationParams)));
}

TArray<FStateGeneratorPreset> AAutomataOrchestrator::GetStateGeneratorPresets() const
{
	return StateGeneratorPresets::GetAll();
}

void AAutomataOrchestrator::ApplyStateGeneratorPreset(int32 PresetIndex, bool bGenerateImmediately)
{
	const TArray<FStateGeneratorPreset>& Presets = StateGeneratorPresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyStateGeneratorPreset: индекс %d вне диапазона (пресетов: %d)"),
			PresetIndex, Presets.Num());
		return;
	}

	const FStateGeneratorPreset& Preset = Presets[PresetIndex];
	GenerationParams = Preset.Params;

	UE_LOG(LogTemp, Log, TEXT("ApplyStateGeneratorPreset: '%s' (%s) - оценка %lld клеток"),
		*Preset.Name, *Preset.FamilyName, StateGenerators::EstimateCellCount(GenerationParams));

	if (bGenerateImmediately)
	{
		GenerateState();
	}
}

void AAutomataOrchestrator::SetStateGeneratorParams(const FStateGeneratorParams& NewParams)
{
	GenerationParams = NewParams;

	// Клампы повторяют метаданные UPROPERTY: панель их соблюдает, а Blueprint
	// пишет в структуру напрямую и может занести что угодно.
	GenerationParams.Extent.X = FMath::Max(GenerationParams.Extent.X, 1);
	GenerationParams.Extent.Y = FMath::Max(GenerationParams.Extent.Y, 1);
	GenerationParams.Extent.Z = FMath::Max(GenerationParams.Extent.Z, 1);
	GenerationParams.Period.X = FMath::Max(GenerationParams.Period.X, 1);
	GenerationParams.Period.Y = FMath::Max(GenerationParams.Period.Y, 1);
	GenerationParams.Period.Z = FMath::Max(GenerationParams.Period.Z, 1);
	GenerationParams.CoreExtent.X = FMath::Max(GenerationParams.CoreExtent.X, 1);
	GenerationParams.CoreExtent.Y = FMath::Max(GenerationParams.CoreExtent.Y, 1);
	GenerationParams.CoreExtent.Z = FMath::Max(GenerationParams.CoreExtent.Z, 1);
	GenerationParams.BlockSize = FMath::Max(GenerationParams.BlockSize, 1);
	GenerationParams.Thickness = FMath::Max(GenerationParams.Thickness, 1);
	GenerationParams.Radius = FMath::Max(GenerationParams.Radius, 1);
	GenerationParams.Amount = FMath::Max(GenerationParams.Amount, 1);
	GenerationParams.ClusterCount = FMath::Max(GenerationParams.ClusterCount, 1);
	GenerationParams.ClusterRadius = FMath::Max(GenerationParams.ClusterRadius, 1);
	GenerationParams.Density = FMath::Clamp(GenerationParams.Density, 0.0f, 1.0f);
	GenerationParams.ClusterRadiusJitter = FMath::Clamp(GenerationParams.ClusterRadiusJitter, 0.0f, 0.9f);
	GenerationParams.NoiseScale = FMath::Max(GenerationParams.NoiseScale, 0.001f);
	GenerationParams.NoiseThreshold = FMath::Clamp(GenerationParams.NoiseThreshold, -1.0f, 1.0f);

	// FMath::PerlinNoise3D() возвращает ровно 0 в целочисленных точках, так что
	// "круглый" масштаб вырождает поле и даёт либо пустоту, либо сплошной куб -
	// молча отдать пустой результат тут хуже всего.
	if (GenerationParams.Type == EStateGeneratorType::NoisePerlin)
	{
		const float ScaleFraction = FMath::Frac(1.0f / FMath::Max(GenerationParams.NoiseScale, KINDA_SMALL_NUMBER));
		if (ScaleFraction < 0.01f || ScaleFraction > 0.99f)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("SetStateGeneratorParams: NoiseScale %.4f почти целократен - Perlin равен нулю в целых точках, результат может выйти пустым или сплошным"),
				GenerationParams.NoiseScale);
		}
	}
}

int64 AAutomataOrchestrator::EstimateStateGeneratorCells() const
{
	return StateGenerators::EstimateCellCount(GenerationParams);
}

FString AAutomataOrchestrator::GetStateGeneratorDisplayName() const
{
	return StateGenerators::GetDisplayName(GenerationParams.Type);
}

FString AAutomataOrchestrator::EnsureSliceDirectory() const
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AutomataSlices"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	return Dir;
}

bool AAutomataOrchestrator::BuildSliceCapture(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
{
	if (!Grid || Grid->Num() == 0)
	{
		OutError = TEXT("сетка пуста - снимать нечего");
		return false;
	}

	// Тот же сбор, что готовит экранный рендер: клетки уже отфильтрованы кубом,
	// возрастом и срезом, и уже окрашены по рампе. Побочный эффект - перезапись
	// LastRenderStats, поэтому сохраняем и возвращаем: съёмка не должна
	// подменять собой статистику последнего РЕНДЕРА, которую показывает HUD.
	const FCellRenderStats SavedRenderStats = LastRenderStats;

	// Цвета для файла нужны гамма-кодированными, в отличие от экранных - см.
	// FSliceCaptureParams::bEncodeSRGB. Признак снимается на время сбора.
	const bool bSavedCaptureColors = bBuildingSliceCapture;
	bBuildingSliceCapture = SliceCaptureParams.bEncodeSRGB;

	TArray<FCellRenderInstance> Cells;
	BuildCellRenderData(Cells);

	bBuildingSliceCapture = bSavedCaptureColors;
	LastRenderStats = SavedRenderStats;

	if (Cells.Num() == 0)
	{
		OutError = TEXT("после фильтров не осталось ни одной видимой клетки");
		return false;
	}

	CellRasterizer::FRasterParams RasterParams;
	RasterParams.CellSize = Grid->GetCellSize();
	RasterParams.PixelsPerCell = FMath::Max(SliceCaptureParams.PixelsPerCell, 1);
	RasterParams.Mode = SliceCaptureParams.Mode;
	RasterParams.ForegroundColor = SliceCaptureParams.ForegroundColor;
	RasterParams.BackgroundColor = SliceCaptureParams.bTransparentBackground
		? FColor(0, 0, 0, 0)
		: SliceCaptureParams.BackgroundColor;

	// Оси - от камеры, но только как ориентир "с какой стороны": базис
	// округляется до осевого, иначе клетки не легли бы в регулярную сетку.
	FVector CameraForward = FVector::ForwardVector;
	FVector CameraUp = FVector::UpVector;
	if (GamePC && GamePC->PlayerCameraManager)
	{
		const FRotationMatrix CameraBasis(GamePC->PlayerCameraManager->GetCameraRotation());
		CameraForward = CameraBasis.GetUnitAxis(EAxis::X);
		// Верх берётся У КАМЕРЫ, а не из мира: при взгляде сверху вниз мировой
		// "вверх" вырожден, а камерный указывает, где у картинки север.
		CameraUp = CameraBasis.GetUnitAxis(EAxis::Z);
	}

	CellRasterizer::BuildAxes(CameraForward, CameraUp, RasterParams);

	CellRasterizer::FRasterImage Image;
	if (!CellRasterizer::Rasterize(Cells, RasterParams, MaxCapturePixels, Image, OutError))
	{
		return false;
	}

	OutWidth = Image.Width;
	OutHeight = Image.Height;
	OutPixels = MoveTemp(Image.Pixels);
	return true;
}

bool AAutomataOrchestrator::EstimateSliceCaptureSize(int32& OutWidth, int32& OutHeight)
{
	if (!Grid || Grid->Num() == 0)
	{
		return false;
	}

	const FCellRenderStats SavedRenderStats = LastRenderStats;
	TArray<FCellRenderInstance> Cells;
	BuildCellRenderData(Cells);
	LastRenderStats = SavedRenderStats;

	CellRasterizer::FRasterParams RasterParams;
	RasterParams.CellSize = Grid->GetCellSize();
	RasterParams.PixelsPerCell = FMath::Max(SliceCaptureParams.PixelsPerCell, 1);

	FVector CameraForward = FVector::ForwardVector;
	FVector CameraUp = FVector::UpVector;
	if (GamePC && GamePC->PlayerCameraManager)
	{
		const FRotationMatrix CameraBasis(GamePC->PlayerCameraManager->GetCameraRotation());
		CameraForward = CameraBasis.GetUnitAxis(EAxis::X);
		CameraUp = CameraBasis.GetUnitAxis(EAxis::Z);
	}

	CellRasterizer::BuildAxes(CameraForward, CameraUp, RasterParams);
	return CellRasterizer::ComputeImageSize(Cells, RasterParams, OutWidth, OutHeight);
}

bool AAutomataOrchestrator::WriteSliceCaptureToFile(const FString& FilePath)
{
	const double StartSeconds = FPlatformTime::Seconds();

	TArray<FColor> Pixels;
	int32 Width = 0;
	int32 Height = 0;
	FString Error;

	if (!BuildSliceCapture(Pixels, Width, Height, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureTextureSlice: %s"), *Error);
		ShowStatusMessage(StatusKey_SliceCapture, FString::Printf(TEXT("Снимок не сделан: %s"), *Error));
		return false;
	}

	TArray64<uint8> PngBytes;
	FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), PngBytes);

	if (PngBytes.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureTextureSlice: не удалось закодировать PNG"));
		ShowStatusMessage(StatusKey_SliceCapture, TEXT("Снимок не сделан: ошибка кодирования PNG"));
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(PngBytes, *FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureTextureSlice: не удалось записать файл '%s'"), *FilePath);
		ShowStatusMessage(StatusKey_SliceCapture, TEXT("Снимок не сделан: файл не записан"));
		return false;
	}

	const double Seconds = FPlatformTime::Seconds() - StartSeconds;
	const double MegaBytes = static_cast<double>(PngBytes.Num()) / (1024.0 * 1024.0);

	UE_LOG(LogTemp, Log, TEXT("CaptureTextureSlice: %dx%d, %.2f МБ, %.2f мс -> %s"),
		Width, Height, MegaBytes, Seconds * 1000.0, *FilePath);
	ShowStatusMessage(StatusKey_SliceCapture,
		FString::Printf(TEXT("Снимок %dx%d (%.1f МБ): %s"), Width, Height, MegaBytes, *FPaths::GetCleanFilename(FilePath)));

	return true;
}

void AAutomataOrchestrator::CaptureTextureSlice()
{
	// Без диалога намеренно: снимки делают сериями, и ценность в том, что
	// нажатие ничего не спрашивает. Полный путь уходит в лог.
	const FString FileName = FString::Printf(TEXT("Slice_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	WriteSliceCaptureToFile(EnsureSliceDirectory() / FileName);
}

void AAutomataOrchestrator::CaptureTextureSliceAs()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureTextureSliceAs: диалоги недоступны - снимок не сделан"));
		return;
	}

	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	const FString DefaultFileName = FString::Printf(TEXT("Slice_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

	TArray<FString> PickedFiles;
	const bool bPicked = DesktopPlatform->SaveFileDialog(
		ParentWindowHandle,
		TEXT("Сохранить срез"),
		EnsureSliceDirectory(),
		DefaultFileName,
		TEXT("PNG Image (*.png)|*.png"),
		EFileDialogFlags::None,
		PickedFiles);

	if (!bPicked || PickedFiles.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("CaptureTextureSliceAs: отменено"));
		return;
	}

	FString FilePath = FPaths::ConvertRelativePathToFull(PickedFiles[0]);
	if (FPaths::GetExtension(FilePath).IsEmpty())
	{
		FilePath += TEXT(".png");
	}

	WriteSliceCaptureToFile(FilePath);
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

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	// Строим правило заново на каждый вызов, чтобы правки BirthCounts/
	// SurvivalCounts/Neighborhood в Details panel подхватывались немедленно
	// (аналогично тому, как GenerateRandom() каждый раз пересоздаёт Grid,
	// а не кэширует его)
	FCellularAutomatonRule AutomatonRule(BirthCounts, SurvivalCounts, Neighborhood, States);
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

			// StepBatch() вместо Step(): стратегия, умеющая считать несколько
			// поколений за один свой внутренний круг (GPU - см. её
			// doc-comment), берёт столько, сколько может, и говорит, сколько
			// реально продвинула; CPU-стратегия всегда возвращает 1, и цикл
			// вырождается в прежний "по одному поколению за итерацию".
			TUniquePtr<FCellGrid> ResultGrid;
			const FCellGrid* SourceGrid = CurrentGridPtr;
			int32 StepsDone = 0;
			while (StepsDone < NumSteps)
			{
				TUniquePtr<FCellGrid> NextGrid = MakeUnique<FDenseCellGrid>(CellSizeSnapshot, ChunkSizeSnapshot, AutomatonRule.HasDecayStates());
				const int32 StepsAdvanced = ComputeStrategy->StepBatch(*SourceGrid, *NextGrid, AutomatonRule, NumSteps - StepsDone);

				// Оба прохода умеют продвинуть состояние только с одного
				// поколения на СОСЕДНЕЕ, а внутри пачки промежуточных не
				// существует - стратегия, продвинувшая больше одного, обязана
				// была заполнить и возрасты, и угасание сама (см. её
				// doc-comment). Позвать их поверх этого значило бы затереть
				// верные значения неверными.
				if (StepsAdvanced <= 1)
				{
					CellAging::ComputeAges(SourceGrid, *NextGrid);
					CellDecay::AdvanceDecayStates(SourceGrid, *NextGrid, AutomatonRule.GetStates());
				}

				ResultGrid = MoveTemp(NextGrid);
				SourceGrid = ResultGrid.Get();
				StepsDone += FMath::Max(1, StepsAdvanced);
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

				// Тот же отложенный сброс, что и в ApplyStepResult() - см.
				// doc-comment bResetToInitialStatePending.
				if (StrongThis->bResetToInitialStatePending)
				{
					StrongThis->bResetToInitialStatePending = false;
					StrongThis->ResetToInitialState();
					return;
				}

				// Отложенный реролл (N) - см. doc-comment bNewSeedPending.
				if (StrongThis->bNewSeedPending)
				{
					StrongThis->bNewSeedPending = false;
					StrongThis->NewSeed();
					return;
				}

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

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	// Правило, стратегия расчёта и буфер следующего поколения строим здесь,
	// на game thread - все три читают UPROPERTY (BirthCounts/SurvivalCounts/
	// Neighborhood/ComputeMethod/GpuVolumeCellLimit/CellSize/ChunkSize),
	// которые могут одновременно редактироваться в Details panel. После этой
	// точки фоновый поток их больше не касается - только *Grid (на чтение) и
	// NextGridBuffer (на запись, свежесозданный, ни с кем не общий).
	FCellularAutomatonRule AutomatonRule(BirthCounts, SurvivalCounts, Neighborhood, States);
	TUniquePtr<FCellularAutomatonComputeStrategy> ComputeStrategy = CreateComputeStrategy();
	TUniquePtr<FCellGrid> NextGridBuffer = CreateGrid();

	// Сколько поколений посчитать за ОДИН фоновый заход. Больше одного - только
	// если стратегия действительно умеет пачки для этого правила (см.
	// FCellularAutomatonComputeStrategy::SupportsStepBatching()): тогда
	// StepsPerRender поколений считаются за один круг через GPU и рендерится
	// итог, вместо StepsPerRender отдельных заходов, из которых рисуется
	// последний. Если не умеет (CPU-стратегия, либо GPU в режиме Generations) -
	// остаётся ровно прежний ритм "одно поколение за заход", вместе со всей
	// логикой пропуска рендеров по StepsSinceLastRender: собирать поколения в
	// пачку там незачем, работа та же, но одним длинным блоком.
	const int32 BatchGenerations = ComputeStrategy->SupportsStepBatching(AutomatonRule)
		? FMath::Max(1, StepsPerRender)
		: 1;

	// Промежуточные буферы поколений (нужны только при BatchGenerations > 1)
	// создаются уже в фоне, поэтому CellSize/ChunkSize - живые UPROPERTY,
	// которые фоновому потоку трогать нельзя - снимаем здесь. Тот же приём,
	// что в Next().
	const float CellSizeSnapshot = CellSize;
	const int32 ChunkSizeSnapshot = ChunkSize;

	bStepInProgress = true;

	FCellGrid* CurrentGridPtr = Grid.Get();
	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	// CurrentGridPtr - сырой указатель на *Grid, без защиты времени жизни -
	// PendingStepFuture даёт EndPlay() дождаться завершения этого фонового
	// шага перед тем, как актор (а с ним и Grid) начнёт разрушаться.
	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[AutomatonRule = MoveTemp(AutomatonRule), ComputeStrategy = MoveTemp(ComputeStrategy),
		 NextGridBuffer = MoveTemp(NextGridBuffer), CurrentGridPtr, WeakThis,
		 BatchGenerations, CellSizeSnapshot, ChunkSizeSnapshot]() mutable
		{
			const double StepStartSeconds = FPlatformTime::Seconds();

			// При BatchGenerations == 1 (CPU-стратегия / Generations) цикл
			// выполняется ровно один раз и ничего лишнего не аллоцирует -
			// путь остаётся прежним. Тот же порядок вызовов и то же условие
			// пропуска ComputeAges(), что в Next(): продвинувшая больше одного
			// поколения стратегия обязана была заполнить возрасты сама.
			TUniquePtr<FCellGrid> PreviousGrid;
			const FCellGrid* SourceGrid = CurrentGridPtr;
			int32 GenerationsAdvanced = 0;
			while (true)
			{
				const int32 StepsAdvanced = ComputeStrategy->StepBatch(*SourceGrid, *NextGridBuffer, AutomatonRule, BatchGenerations - GenerationsAdvanced);

				if (StepsAdvanced <= 1)
				{
					CellAging::ComputeAges(SourceGrid, *NextGridBuffer);
					CellDecay::AdvanceDecayStates(SourceGrid, *NextGridBuffer, AutomatonRule.GetStates());
				}

				GenerationsAdvanced += FMath::Max(1, StepsAdvanced);

				// Выходим и когда набрали всю пачку, и когда стратегия
				// фактически НЕ пачкует. Второе - не теория: стратегия отвечает
				// на SupportsStepBatching() один раз за заход, а влезает ли
				// пачка, решается уже внутри StepBatch() по текущему объёму
				// AABB, который растёт вместе с сеткой. Дорастив объём до
				// потолка, пачка урезается до 1 - и без этого выхода цикл
				// намолотил бы BatchGenerations одиночных шагов внутри ОДНОГО
				// фонового захода: та же работа, но одним блоком на несколько
				// секунд, с висящим всё это время bStepInProgress (он блокирует
				// R и генерацию) и с прерванным чанковым разливом. Наблюдалось
				// живьём: на 11 млн клеток такой заход занял 7.7 с. Возврат к
				// прежнему ритму "одно поколение за заход" здесь строго лучше -
				// следующее посчитается следующим Tick()'ом.
				if (StepsAdvanced <= 1 || GenerationsAdvanced >= BatchGenerations)
				{
					break;
				}

				// Только что посчитанное поколение становится источником для
				// следующего - и должно оставаться живым, пока в него читают,
				// поэтому владение переезжает в PreviousGrid, а не теряется.
				PreviousGrid = MoveTemp(NextGridBuffer);
				SourceGrid = PreviousGrid.Get();
				NextGridBuffer = MakeUnique<FDenseCellGrid>(CellSizeSnapshot, ChunkSizeSnapshot, AutomatonRule.HasDecayStates());
			}

			const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;

			// Снимаем ещё здесь, пока ComputeStrategy жива (уничтожится вместе
			// с этой лямбдой) - см. FHudStats::EstimatedGpuComputeUploadMB.
			const int64 ComputeUploadBytes = ComputeStrategy->GetLastComputeUploadBytes();

			// Grid/рендер трогаем только на game thread - AsyncTask сюда и
			// маршрутизирует. WeakThis - на случай, если актор уничтожили
			// (например, level unload) пока фоновый Step() ещё считался.
			AsyncTask(ENamedThreads::GameThread, [WeakThis, NextGridBuffer = MoveTemp(NextGridBuffer), StepSeconds, ComputeUploadBytes, GenerationsAdvanced]() mutable
			{
				if (AAutomataOrchestrator* StrongThis = WeakThis.Get())
				{
					StrongThis->ApplyStepResult(MoveTemp(NextGridBuffer), StepSeconds, ComputeUploadBytes, GenerationsAdvanced);
				}
			});
		});
}

void AAutomataOrchestrator::BuildCellRenderData(TArray<FCellRenderInstance>& OutInstances)
{
	OutInstances.Reset();

	// Таблица цвета считается один раз на весь рендер, а не на клетку: при
	// миллионах клеток интерполяция в цикле - это миллионы лишних лерпов,
	// тогда как таблица занимает 1 КБ и даёт одно чтение по индексу.
	TArray<FColor> AgeLut;
	BuildAgeColorLut(AgeLut, bBuildingSliceCapture);

	TArray<FIntVector> AliveCells;

	// Если отсечение активно (см. GetActiveCullVolume()) - отсекаем клетки вне
	// границ куба ДО построения инстансов/трансформов, иначе рендерим всё
	// как раньше.
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	if (CullVolume)
	{
		Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), AliveCells);
	}
	else
	{
		Grid->GetAliveCells(AliveCells);
	}

	// Срез вдоль взгляда - см. bEnableViewSlice. Плоскость среза
	// перпендикулярна направлению камеры, поэтому проверка на клетку это одно
	// скалярное произведение: глубина вдоль взгляда против диапазона.
	// Считается ЗДЕСЬ, а не в рендерере, по той же причине, что и куб: клетки
	// вне среза не должны стоить построения трансформа.
	// Инициализированы явно: GetCameraView() пишет их только при успехе, и
	// хотя читаются они строго под bSliceActive, компилятор этого не выводит.
	FVector SliceOrigin = FVector::ZeroVector;
	FVector SliceForward = FVector::ForwardVector;
	const bool bSliceActive = bEnableViewSlice && GetCameraView(SliceOrigin, SliceForward);
	const float SliceMinDepth = ViewSliceDistance - ViewSliceThickness * 0.5f;
	const float SliceMaxDepth = ViewSliceDistance + ViewSliceThickness * 0.5f;

	if (bSliceActive)
	{
		// Запоминаем, для какой камеры срез построен - по этому состоянию
		// Tick() решает, пора ли перестраивать (см. ShouldRefreshViewSlice()).
		LastViewSliceCameraLocation = SliceOrigin;
		LastViewSliceCameraForward = SliceForward;
		bHasViewSliceCameraState = true;
	}

	// Фильтр по возрасту (см. AgeFilterValues) разворачивается в маску ДО
	// цикла: внутри тогда остаётся одно чтение из таблицы, без перебора
	// выбранных возрастов на каждой из миллионов клеток.
	TArray<bool> AgeFilterMask;
	const bool bAgeFilterActive = BuildAgeFilterMask(AgeFilterMask);

	OutInstances.Reserve(AliveCells.Num());
	for (const FIntVector& Cell : AliveCells)
	{
		const uint8 Age = Grid->GetAge(Cell);
		// Раньше остальных проверок: отсекает больше всего и обходится одним
		// чтением. Возраст 0 - законный слой, выключенному фильтру
		// соответствует пустой список, а не нулевой возраст.
		if (bAgeFilterActive && !AgeFilterMask[Age])
		{
			continue;
		}

		const FVector World = Grid->GridToWorld(Cell);
		if (bSliceActive)
		{
			const double Depth = FVector::DotProduct(World - SliceOrigin, SliceForward);
			if (Depth < SliceMinDepth || Depth > SliceMaxDepth)
			{
				continue;
			}
		}

		OutInstances.Add({ FVector3f(World), AgeLut[Age] });
	}

	// Generations (States > 2) - угасающие клетки (не живые, но ещё не
	// полностью мёртвые, см. FCellGrid::IsDecaying()) тоже нужно рисовать
	// (иначе они просто невидимы, хотя реально "занимают" клетку и угасают
	// на глазах у CellDecay::AdvanceDecayStates()). Цвет берётся из СВОЕЙ
	// таблицы (см. DecayColors) - раньше угасающие шли в те же возрастные
	// бакеты, что и живые, и были от них визуально неотличимы. При States == 2
	// этот блок вообще не выполняется - ни GetDecayingCells()/
	// GetDecayingCellsInBounds(), ни лишний проход, ни построение таблицы.
	// Фильтр по возрасту прячет угасающие клетки целиком: возраст у них не
	// определён - это отдельный канал состояния, а не возраст, и приписать им
	// какой-то возраст значило бы соврать (см. AgeFilterValues).
	if (States > 2 && !IsAgeFilterActive())
	{
		TArray<FColor> DecayLut;
		BuildDecayColorLut(DecayLut, bBuildingSliceCapture);

		TArray<FIntVector> DecayingCells;
		TArray<uint8> DecayingStates;
		if (CullVolume)
		{
			Grid->GetDecayingCellsInBounds(CullVolume->GetWorldBounds(), DecayingCells, DecayingStates);
		}
		else
		{
			Grid->GetDecayingCells(DecayingCells, DecayingStates);
		}

		OutInstances.Reserve(OutInstances.Num() + DecayingCells.Num());
		for (int32 Index = 0; Index < DecayingCells.Num(); ++Index)
		{
			const FVector World = Grid->GridToWorld(DecayingCells[Index]);
			// Тот же срез, что и для живых клеток выше - иначе угасающие
			// торчали бы сквозь него.
			if (bSliceActive)
			{
				const double Depth = FVector::DotProduct(World - SliceOrigin, SliceForward);
				if (Depth < SliceMinDepth || Depth > SliceMaxDepth)
				{
					continue;
				}
			}

			OutInstances.Add({ FVector3f(World), DecayLut[DecayingStates[Index]] });
		}
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
	//
	// RenderedCellCount берётся из ИТОГОВОГО массива, а не из AliveCells:
	// раньше сюда шло AliveCells.Num() уже ПОСЛЕ того, как угасающие клетки
	// были добавлены в бакеты и уходили в AddInstances - при States > 2
	// "отрисовано" систематически занижалось ровно на их число, а вместе с
	// ним и EstimatedUploadMB. Обратная сторона: теперь RenderedCellCount
	// может законно превышать TotalCellCount (Grid->Num() считает только
	// живых) - см. doc-comment FCellRenderStats.
	LastRenderStats.RenderedCellCount = OutInstances.Num();
	LastRenderStats.TotalCellCount = Grid->Num();
	LastRenderStats.BytesPerInstance = (int32)(sizeof(FTransform) + CellCustomDataFloats * sizeof(float));
	LastRenderStats.EstimatedUploadMB = (double(LastRenderStats.RenderedCellCount) * LastRenderStats.BytesPerInstance) / (1024.0 * 1024.0);

	UE_LOG(LogTemp, Log, TEXT("BuildCellRenderData: %d/%d клеток (отрисовано/живых в сетке) - выгрузка в AddInstances ~%.2f МБ (%d байт/инстанс: FTransform + %d float per-instance цвета, без учёта оверхеда HISM/драйвера)"),
		LastRenderStats.RenderedCellCount, LastRenderStats.TotalCellCount,
		LastRenderStats.EstimatedUploadMB, LastRenderStats.BytesPerInstance, CellCustomDataFloats);
}

ARenderCullVolume* AAutomataOrchestrator::EnsureRenderCullVolume()
{
	if (!IsValid(CachedRenderCullVolume))
	{
		CachedRenderCullVolume = Cast<ARenderCullVolume>(UGameplayStatics::GetActorOfClass(GetWorld(), ARenderCullVolume::StaticClass()));
	}
	return CachedRenderCullVolume;
}

ARenderCullVolume* AAutomataOrchestrator::GetActiveCullVolume()
{
	if (!bEnableRenderCullVolume)
	{
		return nullptr;
	}

	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	// Спрятанный куб не режет - см. doc-comment в заголовке.
	return (CullVolume && CullVolume->IsVolumeVisible()) ? CullVolume : nullptr;
}

void AAutomataOrchestrator::RenderGridImmediate()
{
	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("RenderGridImmediate: CellMaterial не назначен - рендер пропущен"));
		return;
	}

	EnsureCellsRenderer();
	ApplyCellCullDistances();
	ApplyCellShadowSettings();
	ClearInactiveCellsMeshComponent();

	if (!CellsRenderer)
	{
		return;
	}

	CellsRenderer->SetMesh(CellMesh);
	CellsRenderer->SetMaterial(CellMaterial);
	// Явно 1.0: тот же класс рендерера используется и для подсветки выделения,
	// где множитель 1.1 (см. SelectionScaleMultiplier). Инвариант "обычные
	// клетки - ровно в размер клетки" лучше держать локально и видимо, чем
	// полагаться на то, что этих двух рендереров никто никогда не смешает.
	CellsRenderer->SetScaleMultiplier(1.0f);

	if (ShouldGhostShapeReplaceDetailedRender())
	{
		// Ghost Shape уже покрывает всю сетку целиком (см. doc-comment
		// ShouldGhostShapeReplaceDetailedRender()) - пропускаем именно ту
		// дорогую работу (BuildCellRenderData()+AddInstances по каждой живой
		// клетке), ради которой эта фича существует. Через Render() с
		// пустым списком инстансов, а не сырой ClearInstances() на компоненте -
		// так внутренняя бухгалтерия рендерера (PendingInstances/PendingCursor)
		// остаётся согласованной с самим компонентом, вместо того чтобы её
		// обходить.
		CellsRenderer->Render(*Grid, TArray<FCellRenderInstance>());
		RenderSelectionOverlay();
		UE_LOG(LogTemp, Log, TEXT("RenderGridImmediate: детальный рендер пропущен - Ghost Shape покрывает всю сетку целиком (%d живых клеток)"),
			Grid->Num());
		return;
	}

	TArray<FCellRenderInstance> Instances;
	BuildCellRenderData(Instances);

	// Всегда одним снимком (не BeginRender()/чанкинг) - Next()/GenerateRandom()
	// рендерят немедленно и целиком, независимо от bEnableChunkedRender
	// (см. doc-comment RenderGridImmediate() в заголовке).
	CellsRenderer->Render(*Grid, MoveTemp(Instances));

	// Не-op, если SelectedCells пуст (свежая сетка/шаг уже его сбросили) -
	// сам чистит SelectionMeshComponent в этом случае.
	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("RenderGridImmediate: живых клеток %d отрисовано одним снимком"),
		Grid->Num());
	// Один кадр по построению - "мс/кадр" здесь совпадает с полной ценой
	// разлива и показывает, во что обошёлся бы отказ от чанкинга.
	LogRenderTimings(TEXT("immediate"), CellsRenderer->GetLastRenderTimings(),
		LastRenderStats.RenderedCellCount, 1);
}

void AAutomataOrchestrator::RenderCurrentGrid()
{
	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("RenderCurrentGrid: CellMaterial не назначен - рендер пропущен"));
		return;
	}

	EnsureCellsRenderer();
	ApplyCellCullDistances();
	ApplyCellShadowSettings();
	ClearInactiveCellsMeshComponent();

	if (!CellsRenderer)
	{
		return;
	}

	CellsRenderer->SetMesh(CellMesh);
	CellsRenderer->SetMaterial(CellMaterial);
	// См. одноимённый комментарий в RenderGridImmediate().
	CellsRenderer->SetScaleMultiplier(1.0f);

	if (ShouldGhostShapeReplaceDetailedRender())
	{
		// Тот же принцип, что в RenderGridImmediate() - см. её doc-comment
		// у аналогичной ветки. bEnableChunkedRender здесь тоже не важен:
		// нет живых инстансов - нечего разливать по кадрам, а если реавил
		// с прошлого поколения ещё шёл, Render() с пустым списком инстансов
		// (через BeginRender()+полный слив внутри) сам обнуляет
		// PendingInstances/PendingCursor - bChunkedRenderInProgress
		// сама подхватит это на следующем Tick()/AdvanceChunkedRender().
		CellsRenderer->Render(*Grid, TArray<FCellRenderInstance>());
		RenderSelectionOverlay();
		UE_LOG(LogTemp, Log, TEXT("RenderCurrentGrid: детальный рендер пропущен - Ghost Shape покрывает всю сетку целиком (%d живых клеток)"),
			Grid->Num());
		return;
	}

	TArray<FCellRenderInstance> Instances;
	BuildCellRenderData(Instances);

	const FVector CameraLocation = (GamePC && GamePC->PlayerCameraManager)
		? GamePC->PlayerCameraManager->GetCameraLocation()
		: FVector::ZeroVector;

	if (bEnableChunkedRender)
	{
		CellsRenderer->BeginRender(*Grid, MoveTemp(Instances), ChunkedRenderOrder, CameraLocation);
	}
	else
	{
		CellsRenderer->Render(*Grid, MoveTemp(Instances));
	}

	// Подсветка выделения - всегда одним снимком (не чанкуется, выделение
	// всегда маленькое подмножество), не-op, если SelectedCells пуст.
	RenderSelectionOverlay();

	if (bEnableChunkedRender)
	{
		bChunkedRenderInProgress = true;
		ChunkedRenderStartSeconds = FPlatformTime::Seconds();
		ChunkedRenderFrameCount = 0;

		UE_LOG(LogTemp, Log, TEXT("RenderCurrentGrid: живых клеток %d - рендер разлит по кадрам"),
			Grid->Num());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("RenderCurrentGrid: живых клеток %d отрисовано"),
			Grid->Num());
		// Чанкинг выключен - всё уехало одним кадром, как в
		// RenderGridImmediate(). Это же и базовая линия "до чанкинга", с
		// которой сравнивается мс/кадр разлитого варианта.
		LogRenderTimings(TEXT("oneshot"), CellsRenderer->GetLastRenderTimings(),
			LastRenderStats.RenderedCellCount, 1);
	}
}

void AAutomataOrchestrator::ApplyStepResult(TUniquePtr<FCellGrid> NewGrid, double StepSeconds, int64 ComputeUploadBytes, int32 GenerationsAdvanced)
{
	// Один фоновый заход мог посчитать сразу несколько поколений (см.
	// BatchGenerations в StepAsync()) - все счётчики ниже считают ПОКОЛЕНИЯ,
	// а не заходы, поэтому идут шагом GenerationsAdvanced. При обычном
	// одиночном шаге это 1, и поведение прежнее.
	const int32 Generations = FMath::Max(1, GenerationsAdvanced);

	// Темп следующих заходов - по ФАКТИЧЕСКОМУ размеру этого, а не по тому,
	// что планировалось до дispatch'а: пачка могла быть урезана внутри
	// стратегии (объём AABB упёрся в её потолок), и тогда ждать
	// StepsPerRender/Speed ради одного посчитанного поколения значило бы
	// замедлить симуляцию ровно в StepsPerRender раз. Так интервал сам
	// сходится к реальности за один заход - в обе стороны.
	LastDispatchGenerations = Generations;

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

	// R, нажатый пока этот шаг ещё считался, был отложен (см. doc-comment
	// bResetToInitialStatePending) - гонка на Grid позади, выполняем его
	// сейчас вместо обычного применения только что посчитанного поколения
	// (которое всё равно тут же было бы перезаписано сбросом).
	if (bResetToInitialStatePending)
	{
		bResetToInitialStatePending = false;
		ResetToInitialState();
		return;
	}

	// То же для N, нажатой во время этого шага - реролл вместо применения
	// только что посчитанного поколения (оно всё равно было бы перезаписано
	// новой случайной сеткой). См. doc-comment bNewSeedPending.
	if (bNewSeedPending)
	{
		bNewSeedPending = false;
		NewSeed();
		return;
	}

	// Реально посчитанные поколения - считаем для HUD независимо от того,
	// пропустит ли StepsSinceLastRender ниже фактический рендер (см.
	// GenerationCount/FHudStats).
	GenerationCount += Generations;

	// Ghost Shape пересчитывается по своему отдельному интервалу поколений,
	// независимо от StepsPerRender - см. план "Ghost Shape".
	if (bEnableGhostShape)
	{
		GhostShapeGenerationsSinceRefresh += Generations;
		if (GhostShapeGenerationsSinceRefresh >= FMath::Max(1, GhostShapeRefreshInterval))
		{
			GhostShapeGenerationsSinceRefresh = 0;
			RefreshGhostShape();
		}
	}

	// Шагом в Generations, а не на единицу: когда заход посчитал целую пачку
	// из StepsPerRender поколений, порог достигается тем же самым условием,
	// и рендерится каждый такой заход - отдельной ветки "пачка рендерит
	// всегда" не нужно.
	StepsSinceLastRender += Generations;
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
		UE_LOG(LogTemp, Log, TEXT("StepAsync: %d поколени(й) за заход, %.2f мс [фоновый поток]"), Generations, StepSeconds * 1000.0);
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
	// Настройка принадлежит профилю рендера - раз её тронули руками, профиль в
	// HUD больше не описывает то, что на экране (см. FHudStats::bRenderPresetModified).
	bRenderPresetModified = true;
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
	// Тот же флаг "профиль тронули руками", что и в SetCellCullingEnabled().
	// ApplyRenderPreset() зовёт этот сеттер сам и сбрасывает флаг уже после.
	bRenderPresetModified = true;
	UE_LOG(LogTemp, Log, TEXT("SetGhostShapeEnabled: Ghost Shape %s"), bEnabled ? TEXT("включён") : TEXT("выключен"));

	// RefreshGhostShape() сам разберётся, что делать: bEnableGhostShape ==
	// false в его собственном guard'е сведётся к ClearGhostShape() - не
	// нужно дублировать эту ветку здесь. Счётчик сбрасываем всегда, чтобы
	// ручное включение сразу пересчитало силуэт, а не ждало остаток
	// GhostShapeRefreshInterval с прошлого раза.
	GhostShapeGenerationsSinceRefresh = 0;

	if (!Grid)
	{
		// Ещё нет сетки (до первого GenerateRandom()) - перерисовывать
		// нечего, следующий GenerateRandom()/Next() сам учтёт актуальное
		// состояние переключателя (тот же ранний выход, что у
		// RefreshRenderCullVolume() ниже).
		return;
	}

	// Сначала детальный путь - в режиме "куба нет" (см. doc-comment
	// ShouldGhostShapeReplaceDetailedRender()) он теперь сам решит, рисовать
	// всё как обычно, или очиститься и уступить место силуэту целиком; затем
	// сам силуэт. Без этого включение/выключение Ghost Shape хоткеем H
	// визуально ничего не меняло бы до следующего реально посчитанного
	// поколения - тот же принцип, что и у RefreshRenderCullVolume() ниже.
	RenderGridImmediate();
	RefreshGhostShape();
}

void AAutomataOrchestrator::SetCellShadowsEnabled(bool bEnabled)
{
	bCellsCastShadows = bEnabled;
	bRenderPresetModified = true;
	UE_LOG(LogTemp, Log, TEXT("SetCellShadowsEnabled: тени от клеток %s"), bEnabled ? TEXT("включены") : TEXT("выключены"));

	// Применяем немедленно, не дожидаясь следующего рендера - SetCastShadow()
	// сама обновляет SceneProxy, ей не нужен новый AddInstances() (ровно та же
	// причина, по которой SetCellCullingEnabled() зовёт ApplyCellCullDistances()).
	ApplyCellShadowSettings();
}

void AAutomataOrchestrator::ApplyCellShadowSettings()
{
	// К ОБОИМ компонентам клеток, а не только к активному - если
	// CellMeshComponentType переключат позже, второй не должен остаться со
	// старой настройкой (то же соображение, что в ApplyCellCullDistances()).
	CellsMeshHierarchical->SetCastShadow(bCellsCastShadows);
	CellsMeshFlat->SetCastShadow(bCellsCastShadows);

	if (SelectionMeshComponent)
	{
		SelectionMeshComponent->SetCastShadow(bCellsCastShadows);
	}
}

void AAutomataOrchestrator::SetBackgroundVisible(bool bVisible)
{
	bShowBackground = bVisible;
	bRenderPresetModified = true;
	UE_LOG(LogTemp, Log, TEXT("SetBackgroundVisible: фон %s"), bVisible ? TEXT("показан") : TEXT("скрыт"));

	ApplyBackgroundVisibility();
}

void AAutomataOrchestrator::ApplyBackgroundVisibility()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Небо и облака НЕ прячем как актёров, а исключаем из основного прохода:
	// bRenderInMainPass выключает только отрисовку в кадр (basepass/прозрачность),
	// оставляя компонент в сцене для всего остального - в том числе для
	// real-time-захвата ASkyLight, который каждый кадр пересобирает кубмап
	// окружающего света ИМЕННО С НЕБА.
	//
	// Здесь и была ловушка. "Просто спрятать небо" гасит и свет, и это не
	// побочный эффект, а прямое следствие настройки уровня: у ASkyLight
	// bRealTimeCapture == true, и исчезнувшее небо оставляет захват без
	// источника - рассеянный свет уходит в ноль вместе с фоном. На замерах в
	// PIE (одна и та же точка камеры) пропадали синие и зелёные клетки, вся
	// картинка сваливалась в один тёплый направленный свет.
	//
	// Заморозка захвата (USkyLightComponent::SetRealTimeCaptureEnabled(false))
	// эту дыру НЕ закрывает - проверено там же и отвергнуто: она не
	// пересобирает кубмап на месте, а ставит пересъёмку в очередь
	// (SetCaptureIsDirty() внутри), и та всё равно отрабатывает по уже пустому
	// небу, замораживая чёрный кубмап. bRenderInMainPass сохраняет освещение
	// полностью. Источники света (ASkyLight/ADirectionalLight) не трогаются
	// вовсе - в этом и весь смысл.
	for (TActorIterator<ASkyAtmosphere> It(World); It; ++It)
	{
		if (USkyAtmosphereComponent* SkyComponent = It->GetComponent())
		{
			SkyComponent->SetRenderInMainPass(bShowBackground);
		}
	}
	for (TActorIterator<AVolumetricCloud> It(World); It; ++It)
	{
		// У AVolumetricCloud нет публичного геттера компонента (в отличие от
		// ASkyAtmosphere::GetComponent()), поэтому ищем по классу.
		if (UVolumetricCloudComponent* CloudComponent = It->FindComponentByClass<UVolumetricCloudComponent>())
		{
			CloudComponent->SetRenderInMainPass(bShowBackground);
		}
	}

	// У AExponentialHeightFog такого переключателя нет, поэтому туман прячем
	// целиком. Проверено в том же замере: на освещении это не сказывается -
	// туман, в отличие от неба, захвату ASkyLight светом не служит.
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		It->SetActorHiddenInGame(!bShowBackground);
	}
}

void AAutomataOrchestrator::RunRenderConsoleCommand(const FString& Command)
{
	// Через контроллер, а не GEngine->Exec(): команды VIEWMODE адресованы
	// вьюпорту конкретного локального игрока, и только этот путь их доставляет
	// (им же слал их прежний хоткей Lit/Unlit). Для r.* разницы нет, поэтому
	// весь список идёт одним путём, без ветвления по типу команды.
	if (GamePC)
	{
		GamePC->ConsoleCommand(Command, /*bWriteToLog=*/false);
		return;
	}

	// Контроллер ещё не готов (до BeginPlay) - r.* всё равно применятся, а
	// VIEWMODE в этот момент и применять некуда.
	if (GEngine)
	{
		GEngine->Exec(GetWorld(), *Command);
	}
}

TArray<FRenderPreset> AAutomataOrchestrator::GetRenderPresets() const
{
	return RenderPresets::GetAll();
}

FString AAutomataOrchestrator::GetActiveRenderPresetName() const
{
	const TArray<FRenderPreset>& Presets = RenderPresets::GetAll();
	return Presets.IsValidIndex(ActiveRenderPresetIndex) ? Presets[ActiveRenderPresetIndex].Name : FString();
}

void AAutomataOrchestrator::ApplyRenderPreset(int32 PresetIndex)
{
	const TArray<FRenderPreset>& Presets = RenderPresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyRenderPreset: нет профиля с индексом %d (всего %d) - ничего не меняем"), PresetIndex, Presets.Num());
		return;
	}

	const FRenderPreset& Preset = Presets[PresetIndex];

	// Движковые cvar'ы. Каждый профиль задаёт весь список целиком, поэтому
	// восстанавливать что-либо от предыдущего не нужно - см. doc-comment
	// FRenderPreset::ConsoleCommands.
	for (const FString& Command : Preset.ConsoleCommands)
	{
		RunRenderConsoleCommand(Command);
	}
	RunRenderConsoleCommand(Preset.bLit ? TEXT("VIEWMODE LIT") : TEXT("VIEWMODE UNLIT"));

	// Настройки клеток. Пишем поля напрямую, а не через сеттеры: каждый из них
	// сам дёргает применение и перерисовку, и пройти по ним подряд означало бы
	// три-четыре полных RenderGridImmediate() на одно нажатие клавиши. Ниже
	// всё применяется по разу.
	bCellsCastShadows = Preset.bCellsCastShadows;
	bEnableCellCulling = Preset.bCellCullingEnabled;
	CellCullStartDistance = Preset.CellCullStartDistance;
	CellCullEndDistance = Preset.CellCullEndDistance;
	bShowBackground = Preset.bShowBackground;

	ApplyCellShadowSettings();
	ApplyCellCullDistances();
	ApplyBackgroundVisibility();

	// Ghost Shape - последним и через сеттер: он единственный меняет САМ набор
	// рисуемых объектов (без куба отсечения силуэт заменяет поклеточный рендер
	// целиком), и его сеттер уже делает ровно то, что здесь нужно - перерисовать
	// текущее состояние и пересобрать силуэт, не дожидаясь нового поколения.
	SetGhostShapeEnabled(Preset.bGhostShapeEnabled);

	ActiveRenderPresetIndex = PresetIndex;
	// Строго после SetGhostShapeEnabled() и прочих сеттеров: каждый из них
	// поднимает этот флаг ("настройку профиля тронули руками"), и сбрасывать
	// его нужно уже по итогам всего применения.
	bRenderPresetModified = false;

	UE_LOG(LogTemp, Log, TEXT("ApplyRenderPreset: профиль рендера -> %s (%s)"), *Preset.Name, *Preset.Description);
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
	// заново пройти BuildCellRenderData()/AddInstances() для текущего состояния
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

void AAutomataOrchestrator::MoveCullVolumeToSelection()
{
	// Все отказы ниже сообщаются и на экран, а не только в лог: без этого
	// нажатие K с пустым выделением выглядит как сломанная клавиша - ровно
	// та же жалоба, что была про срез вдоль взгляда.
	if (SelectedCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeToSelection: выделение пусто - сначала выделите клетку (Tab, затем ЛКМ)"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("[K] Выделение пусто - сначала Tab, затем ЛКМ по клетке"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeToSelection: сетка не инициализирована"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("[K] Сетка не инициализирована"));
		return;
	}

	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	if (!CullVolume)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeToSelection: на уровне нет ARenderCullVolume - разместите его сначала"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("[K] На уровне нет ARenderCullVolume - разместите его"));
		return;
	}

	// Только первая выделенная клетка - куб один, центрировать его
	// одновременно на нескольких точках невозможно, а первая обычно и есть
	// та, с которой начали выделение/клик.
	const FIntVector& TargetCell = SelectedCells[0];
	const FVector TargetLocation = Grid->GridToWorld(TargetCell);
	CullVolume->SetActorLocation(TargetLocation);

	UE_LOG(LogTemp, Log, TEXT("MoveCullVolumeToSelection: куб отсечения перемещён к клетке %s (мир: %s)"),
		*TargetCell.ToString(), *TargetLocation.ToString());
	// Отдельно сообщаем, если куб сейчас не режет: он честно переехал, но на
	// экране ничего не изменится, и это выглядело бы как несработавшая
	// клавиша. Условие берём из GetActiveCullVolume() - там же, где его
	// проверяет рендер, чтобы сообщение не разошлось с поведением.
	const bool bCullingActive = GetActiveCullVolume() != nullptr;
	ShowStatusMessage(StatusKey_CullVolume, FString::Printf(TEXT("[K] Куб отсечения по центру клетки %s%s"),
		*TargetCell.ToString(),
		bCullingActive ? TEXT("") : TEXT("   (отсечение НЕ активно - C включить, Ctrl+C показать куб)")));

	// SetActorLocation() программно не триггерит ARenderCullVolume::
	// PostEditMove() (WITH_EDITOR-only, реагирует только на ручное
	// перетаскивание/правку в Details panel) - перерисовываем сами, тем же
	// путём, что и хоткей C/сам PostEditMove().
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::MoveCullVolumeByCells(const FIntVector& CellDelta)
{
	if (CellDelta.IsZero())
	{
		return;
	}

	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	if (!CullVolume)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeByCells: на уровне нет ARenderCullVolume - разместите его сначала"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("Стрелки: на уровне нет ARenderCullVolume - разместите его"));
		return;
	}

	const FVector WorldDelta = FVector(CellDelta) * CellSize;
	const FVector NewLocation = CullVolume->GetActorLocation() + WorldDelta;
	CullVolume->SetActorLocation(NewLocation);

	// Как и в MoveCullVolumeToSelection(): программный SetActorLocation() не
	// поднимает PostEditMove(), так что перерисовываем сами. Сообщение тоже
	// оттуда - куб мог переехать, но если отсечение не активно, на экране
	// ничего не изменится, и это неотличимо от несработавшей клавиши.
	const bool bCullingActive = GetActiveCullVolume() != nullptr;
	ShowStatusMessage(StatusKey_CullVolume, FString::Printf(TEXT("Куб отсечения: %s%s"),
		*NewLocation.ToCompactString(),
		bCullingActive ? TEXT("") : TEXT("   (отсечение НЕ активно - C включить, Ctrl+C показать куб)")));

	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::AdvanceChunkedRender()
{
	++ChunkedRenderFrameCount;

	// Бюджет ChunkedRenderCellsPerFrame уходит единственному рендереру
	// ЦЕЛИКОМ - прежнее деление между возрастными бакетами исчезло вместе с
	// ними (см. doc-comment AdvanceChunkedRender() в заголовке).
	if (CellsRenderer && CellsRenderer->AdvanceRenderChunk(ChunkedRenderCellsPerFrame))
	{
		return;
	}

	bChunkedRenderInProgress = false;

	const double TotalSeconds = FPlatformTime::Seconds() - ChunkedRenderStartSeconds;
	UE_LOG(LogTemp, Log, TEXT("AdvanceChunkedRender: рендер разлитый по кадрам завершён - живых клеток %d за %d кадр(ов)/%.2f мс"),
		Grid ? Grid->Num() : 0, ChunkedRenderFrameCount, TotalSeconds * 1000.0);

	// TotalSeconds выше - это стена от BeginRender() до последнего чанка, т.е.
	// в основном время самих кадров, а не работы рендера. Полезная для
	// подбора ChunkedRenderCellsPerFrame величина - только в разбивке ниже.
	if (CellsRenderer)
	{
		LogRenderTimings(TEXT("chunked"), CellsRenderer->GetLastRenderTimings(),
			LastRenderStats.RenderedCellCount, ChunkedRenderFrameCount);
	}
}

void AAutomataOrchestrator::FinishChunkedRenderImmediately()
{
	if (!bChunkedRenderInProgress)
	{
		return;
	}

	while (CellsRenderer && CellsRenderer->AdvanceRenderChunk(TNumericLimits<int32>::Max()))
	{
	}

	bChunkedRenderInProgress = false;

	UE_LOG(LogTemp, Log, TEXT("FinishChunkedRenderImmediately: чанковый рендер довершён одним разом (остановлен через Stop) - живых клеток %d"),
		Grid ? Grid->Num() : 0);
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

	// bEnableViewSlice - ещё один потребитель тика помимо симуляции: срез
	// следит за камерой и на паузе (см. SetViewSliceEnabled()).
	if (!bSimulationRunning && !bChunkedRenderInProgress && !bEnableViewSlice)
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

	// Не безусловный false: срез вдоль взгляда следит за камерой и на паузе,
	// а без тика он застыл бы (см. SetViewSliceEnabled()).
	SetActorTickEnabled(bEnableViewSlice);
}

void AAutomataOrchestrator::Clear()
{
	
}