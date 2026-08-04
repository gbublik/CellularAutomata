// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "Automata/Rendering/FilteredCellGridView.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/FloatingPawnMovement.h"
// GetMax2DTextureDimension() - предел стороны снимка, см. TakePhotoShot().
#include "RHIGlobals.h"
// RHIGetTextureMemoryStats() - расход видеопамяти в лог при съёмке.
#include "DynamicRHI.h"
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
#include "Automata/Capture/CapturePresets.h"
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

	// Стартуем ТЕМ генератором, что выбран в GenerationParams (то же, что даёт
	// хоткей Y). Случайный шар - не отдельный путь, а обычное значение
	// EStateGeneratorType::RandomBall, поэтому и запасной ветки здесь больше
	// нет: падать было бы некуда и незачем.
	//
	// GenerateState() умеет ОТКАЗАТЬСЯ - по бюджету MaxGeneratedCells или на
	// ошибке генератора - и оставить сетку нетронутой. На старте это значит
	// пустой мир, но зато с внятным сообщением на экране (см. ShowStatusMessage()
	// внутри), а не с молча подменённой фигурой: если настройки генератора
	// заведомо не влезают, честнее показать это сразу, чем нарисовать что-то
	// другое и оставить пользователя гадать, почему.
	GenerateState();

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

	// Съёмка происходит в отрисовке вьюпорта, а она идёт ПОСЛЕ тика актёров в
	// том же кадре - поэтому ждём смены номера кадра, иначе итог подводился бы
	// до затвора. См. PhotoShotIssuedFrame.
	if (PhotoShotIssuedSeconds > 0.0 && GFrameCounter > PhotoShotIssuedFrame)
	{
		ReportPhotoShotCompleted();
	}

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

	// Серия в быстром режиме идёт без пауз между шагами: Speed задаёт темп для
	// ПРОСМОТРА, а съёмке он только мешает - файлы получатся те же самые, но
	// ждать придётся во столько раз дольше. Следующий заход всё равно стартует
	// не раньше, чем закончится предыдущий (bStepInProgress).
	const bool bSeriesRush = bSeriesCaptureActive && SliceCaptureParams.bSeriesFastMode;

	if (bFastStepActive)
	{
		TimeSinceLastStep += DeltaTime;
		const float StepInterval = GenerationsPerDispatch / FMath::Max(Speed, KINDA_SMALL_NUMBER);

		if ((bSeriesRush || TimeSinceLastStep >= StepInterval) && !bStepInProgress && !bBlockedByChunkedRender)
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
	if ((bSeriesRush || TimeSinceLastStep >= StepInterval) && !bStepInProgress && !bBlockedByChunkedRender)
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
	LastHudStats.CellBorderWidth = CellBorderWidth;

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
	LastHudStats.bHeadlightEnabled = GamePC ? GamePC->IsHeadlightEnabled() : false;
	LastHudStats.bAutoReseedOnExtinction = bAutoReseedOnExtinction;
	LastHudStats.AutoReseedCount = AutoReseedCount;
	LastHudStats.ComputeMethod = ComputeMethod;
	LastHudStats.bChunkedRenderEnabled = bEnableChunkedRender;
	LastHudStats.ChunkedRenderOrder = ChunkedRenderOrder;
	LastHudStats.bWaitForChunkedRenderToFinish = bWaitForChunkedRenderToFinish;
	LastHudStats.bCellCullingEnabled = bEnableCellCulling;
	LastHudStats.bRenderCullVolumeEnabled = bEnableRenderCullVolume;
	LastHudStats.bViewSliceEnabled = bEnableViewSlice;
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
	// уровне вовсе), поэтому итоговое берётся из того же GetActiveCullVolume(),
	// что и весь рендер - HUD не повторяет условие своей копией, иначе они
	// могли бы разойтись. Видимость куба - третье, независимое поле: на
	// отсечение она не влияет.
	const ARenderCullVolume* AnyCullVolume = EnsureRenderCullVolume();
	LastHudStats.bRenderCullVolumeVisible = AnyCullVolume && AnyCullVolume->IsVolumeVisible();
	LastHudStats.bCullVolumeActive = GetActiveCullVolume() != nullptr;
	LastHudStats.bGhostShapeReplacesDetailedRender = ShouldGhostShapeReplaceDetailedRender();

	// Два других "режет ли на самом деле" из той же тройки индикаторов - см.
	// bCellCullingActive/bViewSliceActive. Условия повторяют те, что стоят на
	// боевых путях: у отсечения по расстоянию - ApplyCellCullDistances()
	// (нулевая дистанция это движковое "выключено"), у среза -
	// BuildCellRenderData() (плоскость задаётся камерой, без неё резать нечем).
	LastHudStats.bCellCullingActive = bEnableCellCulling && CellCullEndDistance > 0.0f;

	FVector SliceOrigin;
	FVector SliceForward;
	LastHudStats.bViewSliceActive = bEnableViewSlice && GetCameraView(SliceOrigin, SliceForward);

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

	// Новый прогон - новый график. Единственная воронка всех пяти путей
	// "начать заново", см. doc-comment функции.
	GenerationSamples.Reset();
}

void AAutomataOrchestrator::AppendGenerationSample()
{
	// Grid может не быть вовсе - BakeCellsToMesh() его освобождает.
	GenerationHistory::Append(GenerationSamples, GenerationCount,
		Grid.IsValid() ? Grid->Num() : 0, GenerationHistoryCapacity);
}

void AAutomataOrchestrator::NoteRenderedCells(int32 RenderedCount)
{
	GenerationHistory::NoteRendered(GenerationSamples, GenerationCount,
		Grid.IsValid() ? Grid->Num() : 0, RenderedCount, GenerationHistoryCapacity);
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
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, ColorRampSpace)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, ColorRampCurve)
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
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellBorderWidth))
	{
		// Намеренно НЕ перерисовываем: ширина канта живёт в uniform-буфере
		// материала, а не в per-instance данных, поэтому достаточно записать её
		// в динамический инстанс. Тянуть сюда RenderGridImmediate() было бы
		// прямым вредом - на миллионах клеток каждое движение ползунка в
		// Details panel стоило бы полного AddInstances().
		EnsureCellMaterialInstance();
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
	else if (bAutoGenerateOnParamChange && IsGenerationProperty(PropertyChangedEvent))
	{
		// Interactive приходит на КАЖДОМ кадре протяжки ползунка, а генерация
		// стирает сетку и заново заливает инстансы: на миллионах клеток одна
		// протяжка радиуса от края до края означала бы сотню полных
		// перестроений и намертво повешенный редактор. Ждём отпускания
		// (ValueSet) - тогда за жест платим ровно один раз.
		if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
		{
			// Через публичный GenerateState(), а не своим путём: оценка против
			// MaxGeneratedCells, отказ при фоновом шаге и точка возврата для R
			// обязаны быть теми же, что у Y. Отказ здесь безвреден - он
			// оставляет текущее состояние целым и пишет причину на экран.
			GenerateState();
		}
	}
}

bool AAutomataOrchestrator::IsGenerationProperty(const FPropertyChangedEvent& PropertyChangedEvent)
{
	// GenerationParams - структура, и GetPropertyName() отдаёт имя поля ВНУТРИ
	// неё (Radius, Amount, Type...), а не саму структуру. Спрашиваем внешний
	// член: одна проверка покрывает все поля разом, включая те, которых в
	// FStateGeneratorParams ещё нет. Для обычного свойства MemberProperty
	// совпадает с самим свойством, так что Seed ловится тем же кодом.
	const FName MemberName = PropertyChangedEvent.MemberProperty
		? PropertyChangedEvent.MemberProperty->GetFName()
		: PropertyChangedEvent.GetPropertyName();

	// Seed здесь наравне с параметрами: он входит в ту же формулу и правится в
	// той же петле подбора. MaxGeneratedCells, наоборот, НЕ входит - это
	// предохранитель, и его подъём означает "разреши построить", а не "построй
	// прямо сейчас"; перестроение по нему запускало бы ровно ту генерацию,
	// которую предохранитель только что отказался пропускать.
	return MemberName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, GenerationParams)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, Seed);
}
#endif

void AAutomataOrchestrator::NewSeed()
{
	if (bStepInProgress)
	{
		// Не отказываем молча (GenerateState() ниже всё равно откажется -
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
	// Тем же генератором, что и старт с хоткеем Y: N теперь означает "та же
	// фигура, другой сид", а не "случайный шар вместо того, что настроено".
	// Прежнее поведение доступно выбором EStateGeneratorType::RandomBall.
	GenerateState();
}

bool AAutomataOrchestrator::TryAutoReseedOnExtinction(int32 GenerationsAdvanced)
{
	if (!bAutoReseedOnExtinction || !Grid.IsValid() || Grid->Num() != 0)
	{
		return false;
	}

	++AutoReseedCount;

	// Сколько поколений прожил сид, пишем ДО реролла - это единственное, что
	// про него интересно, а GenerationCount вот-вот обнулится вместе с сеткой.
	// Прибавка здесь потому, что вызывающая сторона до своего счётчика ещё не
	// дошла: проверка стоит раньше, чтобы не платить за рендер пустоты.
	UE_LOG(LogTemp, Log, TEXT("Автоперекат сида: сид %d вымер на поколении %lld, попытка №%d"),
		Seed, GenerationCount + GenerationsAdvanced, AutoReseedCount);

	NewSeed();
	return true;
}

void AAutomataOrchestrator::SetAutoReseedOnExtinction(bool bEnable)
{
	bAutoReseedOnExtinction = bEnable;
	AutoReseedCount = 0;

	if (!bEnable)
	{
		UE_LOG(LogTemp, Log, TEXT("SetAutoReseedOnExtinction: автоперекат сида выключен"));
		ShowStatusMessage(StatusKey_Generation, TEXT("Автоперекат сида: выключен"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("SetAutoReseedOnExtinction: автоперекат сида включён - вымершая сетка будет пересеиваться сама"));
	ShowStatusMessage(StatusKey_Generation, TEXT("Автоперекат сида: включён - вымершая сетка пересеивается сама"));

	// Режим включают в том числе ПОСЛЕ того, как всё погасло (смотрел, как
	// умирает, и решил перебирать дальше). Ждать в этом случае нечего: шагов
	// больше не будет - мёртвая сетка мертва и на следующем поколении, - так что
	// первый сид катим прямо здесь. NewSeed() сам отложится, если прямо сейчас
	// считается фоновый шаг (bNewSeedPending).
	if (Grid.IsValid() && Grid->Num() == 0)
	{
		++AutoReseedCount;
		NewSeed();
	}

	// И сразу запускаем прогон, если он не идёт: на паузе перебирать нечего -
	// вымирает только то, что считается. "Включил перебор" и "начал
	// перебирать" - одно действие, а не два, иначе Shift+N в самом типичном
	// случае (пришёл к мёртвой сетке, включил режим) внешне не делал бы ничего.
	//
	// Автошаг по Shift+F не трогаем: он тоже считает поколения, то есть перебор
	// уже идёт, а Start() при нём всё равно откажется (см. его реализацию).
	if (!bSimulationRunning && !IsFastStepActive())
	{
		Start();
	}
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
		GetNeighborhoodDisplayName(Neighborhood));

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
		// Настройки спавна из пресета едут в параметры ГЕНЕРАТОРА, а не в
		// отдельный блок Automata|Random - того больше нет. Тип выставляется
		// явно: пресеты каталога описывают именно случайный шар заданной
		// плотности, и применить их радиус к, скажем, решётке из плит значило бы
		// молча сменить смысл числа.
		GenerationParams.Type = EStateGeneratorType::RandomBall;
		GenerationParams.Radius = Preset.SpawnRadius;
		GenerationParams.Amount = Preset.Amount;
	}

	UE_LOG(LogTemp, Log, TEXT("ApplyRulePreset: '%s' (%s)%s"),
		*Preset.Name, *Preset.RuleString,
		bApplySpawnSettings
			? *FString::Printf(TEXT(", Radius=%d, Amount=%d"), GenerationParams.Radius, GenerationParams.Amount)
			: TEXT(""));
}

TArray<FCellShapePreset> AAutomataOrchestrator::GetCellShapePresets() const
{
	return CellShapePresets::GetAll();
}

void AAutomataOrchestrator::ApplyCellShapePreset(int32 PresetIndex)
{
	const TArray<FCellShapePreset>& Presets = CellShapePresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyCellShapePreset: индекс %d вне диапазона (форм: %d)"), PresetIndex, Presets.Num());
		return;
	}

	const FCellShapePreset& Preset = Presets[PresetIndex];

	// Гексагональная призма требует скошенного отображения в мир, которого
	// сейчас нет. Отказываемся вслух, а не выставляем настройки, которые
	// нарисуют шестиугольники на кубической решётке - это выглядело бы
	// правдоподобно и было бы неверно, ровно та ошибка, из-за которой прошлая
	// попытка гекс-решётки была отменена.
	if (Preset.bRequiresCustomMesh && Preset.ExpectedMeshAabb.Y > Preset.ExpectedMeshAabb.X + UE_KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyCellShapePreset: '%s' требует скошенной решётки, которая ещё не реализована"), *Preset.Name);
		ShowStatusMessage(StatusKey_CellShape, FString::Printf(
			TEXT("Форма '%s' ещё не поддержана: нужна скошенная решётка"), *Preset.Name));
		return;
	}

	// Пишем поля напрямую, а не через сеттеры: каждый из них перерисовывает
	// сетку, и четыре вызова стоили бы трёх лишних полных рендеров на
	// миллионах инстансов. Рендер один, в самом конце.
	GenerationParams.ParityFilter = Preset.ParityFilter;
	Neighborhood = Preset.Neighborhood;
	NeighborhoodShape = Preset.NeighborhoodShape;
	LatticeZScale = Preset.LatticeZScale;
	CellMeshScaleMultiplier = Preset.CellMeshScaleMultiplier;
	ActiveCellShapePresetIndex = PresetIndex;

	// Движок пропорции меша не проверяет никак: неверный ассет даёт щели или
	// наложение, а это неотличимо от неверно выбранной решётки. Поэтому
	// сверяем и говорим вслух - в статус-строку, а не только в лог (прецедент
	// - CellMaterial без ноды PerInstanceCustomData3Vector, который молча не
	// работает).
	FString MeshWarning;
	if (CellMesh)
	{
		const FVector MeshAabb = CellMesh->GetBounds().BoxExtent * 2.0;
		if (!MeshAabb.IsNearlyZero())
		{
			// Сравниваем ПРОПОРЦИИ, а не абсолютный размер: рендерер всё равно
			// нормирует меш по его X-габариту, так что важно лишь отношение
			// сторон.
			const FVector Normalized = MeshAabb / MeshAabb.X;
			const FVector Expected = Preset.ExpectedMeshAabb / Preset.ExpectedMeshAabb.X;
			if (!Normalized.Equals(Expected, 0.01))
			{
				MeshWarning = FString::Printf(TEXT(" | меш имеет пропорции %.2f:%.2f:%.2f вместо %.2f:%.2f:%.2f"),
					Normalized.X, Normalized.Y, Normalized.Z, Expected.X, Expected.Y, Expected.Z);
				UE_LOG(LogTemp, Warning, TEXT("ApplyCellShapePreset: '%s' ожидает меш с габаритом %s, а назначенный имеет %s - будут щели или наложение"),
					*Preset.Name, *Preset.ExpectedMeshAabb.ToCompactString(), *MeshAabb.ToCompactString());
			}
		}
	}

	// Число соседей берётся тем же способом, что и симуляцией (BuildRule()), а
	// не пересчётом из пресета: если они разойдутся, увидеть это надо здесь, а
	// не по странной картинке.
	const int32 ActualNeighborCount = BuildNeighborOffsetsForAnalysis().Num();
	UE_LOG(LogTemp, Log, TEXT("ApplyCellShapePreset: '%s' - %d граней, соседей %d, чётность %d, Z x%.3f, меш x%.1f"),
		*Preset.Name, Preset.FaceCount, ActualNeighborCount, static_cast<int32>(Preset.ParityFilter),
		Preset.LatticeZScale, Preset.CellMeshScaleMultiplier);
	if (ActualNeighborCount != Preset.FaceCount)
	{
		// Одна грань на соседа - это определение ячейки Вороного, а не
		// пожелание. Расхождение значит, что узор растёт туда, где клетки
		// визуально не соприкасаются (или наоборот).
		UE_LOG(LogTemp, Warning, TEXT("ApplyCellShapePreset: у формы '%s' %d граней, а соседей %d - рост не совпадёт с видимыми контактами"),
			*Preset.Name, Preset.FaceCount, ActualNeighborCount);
	}

	ShowStatusMessage(StatusKey_CellShape, FString::Printf(TEXT("Форма клетки: %s (%d граней)%s"),
		*Preset.Name, Preset.FaceCount, *MeshWarning));

	// Решётка поменялась - сетку надо построить заново: старая хранит прежний
	// шаг внутри себя, и клетки в ней стоят по прежней геометрии.
	GenerateState();
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
	// Сперва - в HUD, если он готов принять. Движковый канал рисует строки от
	// зашитых константой 45 пикселей сверху и залезает на верхнюю панель.
	if (UMainHudWidget* Widget = GetHudWidget())
	{
		static const FName StatusEventName(TEXT("OnStatusMessage"));
		const UFunction* Handler = Widget->GetClass()->FindFunctionByName(StatusEventName);

		// У BlueprintImplementableEvent без реализации в графе владелец
		// найденной UFunction - нативный класс (сама заглушка). Как только
		// событие разведено в WBP, у Blueprint-класса появляется своя версия, и
		// поиск от наследника находит именно её. Без этой проверки правка была
		// бы не добавочной: невыведенное событие - это молчаливый no-op, и
		// сообщения исчезли бы совсем, ничего не сказав.
		if (Handler && Handler->GetOwnerClass() && !Handler->GetOwnerClass()->HasAnyClassFlags(CLASS_Native))
		{
			Widget->OnStatusMessage(FText::FromString(Message), Key);
			return;
		}
	}

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

FLinearColor AAutomataOrchestrator::SampleColorRamp(const TArray<FLinearColor>& Keys, float T) const
{
	return ColorRamp::Sample(Keys, T, ColorRampSpace, ColorRampCurve);
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
	// Тот же динамический инстанс, что и у обычных клеток: подсветка отличается
	// только цветом из custom data, и кант на ней должен быть такой же ширины.
	SelectionRenderer->SetMaterial(EnsureCellMaterialInstance());
	// Чуть крупнее обычной клетки - иначе поверхности совпадают и мерцают
	// (z-fighting), см. doc-comment SelectionScaleMultiplier.
	//
	// Множитель клетки обязателен множителем, а не заменой: SelectionScaleMultiplier
	// задан ОТНОСИТЕЛЬНО клетки ("на 10% крупнее"), а не абсолютно. Пока
	// CellMeshScaleMultiplier был единицей, разницы не было; с ячейками решёток,
	// которым нужен масштаб 2 (ромбододекаэдр, усечённый октаэдр), подсветка
	// рисовалась вдвое МЕНЬШЕ клетки и целиком пряталась внутри неё - выделение
	// при этом работало, просто его не было видно.
	SelectionRenderer->SetScaleMultiplier(CellMeshScaleMultiplier * SelectionScaleMultiplier);

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
	// Запас - НАИБОЛЬШИЙ габарит клетки: на решётке, растянутой по оси,
	// занижение до шага в плоскости давало бы недолёт луча вдоль вытянутой
	// оси, то есть промах по последней клетке.
	const double MaxDistance = FVector::Distance(RayOrigin, BoundsCenter) + BoundsRadius + Grid->GetLattice().GetMaxCellWorldExtent();

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

	const FVector ChunkWorldExtent = Grid->GetChunkWorldExtent();
	if (ChunkWorldExtent.GetMin() <= 0.0)
	{
		// Сетка без чанков - выбирать нечего (см. FCellGrid::GetChunkWorldExtent()).
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
	const double MaxDistance = FVector::Distance(RayOrigin, BoundsCenter) + BoundsRadius + ChunkWorldExtent.GetMax();

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

	const FChunkGridView ChunkView(ChunkWorldExtent, Grid->GetLattice().GetCellWorldExtent(), MoveTemp(OccupiedChunks), /*bBuildOccupancySet=*/true);

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
	const FVector ChunkWorldExtent = Grid->GetChunkWorldExtent();
	if (OccupiedChunks.Num() == 0 || ChunkWorldExtent.GetMin() <= 0.0)
	{
		// Нулевой габарит - грид не поддерживает чанкинг (см. doc-comment
		// FCellGrid::GetChunkWorldExtent()) - фича молча ничего не делает.
		ClearGhostShape();
		return;
	}

	// Куб отсечения активен (см. GetActiveCullVolume()) - оставляем только
	// чанки СНАРУЖИ куба, внутри уже рисует обычный детальный путь
	// (BuildCellRenderData()), силуэт здесь чистое дополнение.
	// Иначе (куб выключен хоткеем C либо его вообще нет на уровне) -
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
			const FVector ChunkOrigin = FVector(ChunkCoord) * ChunkWorldExtent;
			const FBox ChunkBounds(ChunkOrigin, ChunkOrigin + ChunkWorldExtent);
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
	FChunkGridView ChunkView(ChunkWorldExtent, Grid->GetLattice().GetCellWorldExtent(), ChunksToGhost);
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

	// Нет активной границы отсечения (куб выключен либо на уровне его вообще
	// нет - см. GetActiveCullVolume()) - RefreshGhostShape() в
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
		// файл не загружался) - нет сохранённой точки возврата, поэтому
		// строим заново тем же генератором, что и старт. На практике сюда не
		// попадают: BeginPlay() зовёт GenerateState(), а тот заполняет
		// InitialStateCells - ветка защитная.
		GenerateState();
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

void AAutomataOrchestrator::StepBackward()
{
	if (bStepInProgress)
	{
		// Откладываем, а не отказываем - как R и N (см. doc-comment
		// bStepBackwardPending). Оба флага гасим: последнее нажатие выигрывает.
		bStepBackwardPending = true;
		bResetToInitialStatePending = false;
		bNewSeedPending = false;
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: фоновый шаг ещё считается - шаг назад отложен до его завершения"));
		return;
	}

	if (GenerationCount <= 0)
	{
		ShowStatusMessage(StatusKey_StepBackward, TEXT("Шаг назад: уже на поколении 0"));
		return;
	}

	if (InitialStateCells.Num() == 0)
	{
		// Без точки возврата пересчитывать не от чего. На практике недостижимо -
		// BeginPlay() зовёт GenerateState(), а тот заполняет InitialStateCells.
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: изначальный узор не сохранён - откатывать не от чего"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: активный CellsMesh-компонент отсутствует"));
		return;
	}

	// Непрерывный прогон и откат несовместимы: Tick() запустил бы следующий
	// StepAsync() сразу после того, как пересчёт вернёт предыдущее поколение, и
	// нажатие выглядело бы несработавшим (сетка мигнула бы назад и тут же ушла
	// вперёд). Останавливаем прогон, а не ставим на паузу - Pause() в этом
	// проекте про управление камерой, симуляцию останавливает Stop().
	if (bSimulationRunning)
	{
		Stop();
	}

	// Автошаг по удержанию Shift+F - второй потребитель той же ветки Tick() и
	// ровно та же проблема: он не гейтится bSimulationRunning, так что одного
	// Stop() выше недостаточно.
	if (bFastStepActive)
	{
		StopFastStep();
	}

	const int64 TargetGeneration = GenerationCount - 1;

	// Поколение 0 - это ровно InitialStateCells, считать нечего.
	if (TargetGeneration == 0)
	{
		ResetToInitialState();
		ShowStatusMessage(StatusKey_StepBackward, TEXT("Шаг назад: поколение 0 (изначальный узор)"));
		return;
	}

	// Новый прогон убирает запечённый меш-снимок и призрачную оболочку - как
	// ResetToInitialState() и GenerateState().
	ClearBakedMesh();
	ClearGhostShape();

	// Засев строим ЗДЕСЬ, на игровом потоке (CreateGrid() читает живые
	// UPROPERTY), и отдаём его в фон по значению - Grid при этом не трогаем
	// вовсе: пока идёт пересчёт, на экране остаётся текущее поколение, а
	// подменится оно разом в продолжении. Тем же самым это отличается от
	// ResetToInitialState(), который рисует изначальный узор немедленно.
	TUniquePtr<FCellGrid> SeedGrid = CreateGrid();
	for (const FIntVector& Cell : InitialStateCells)
	{
		SeedGrid->SetAlive(Cell, true);
		SeedGrid->SetAge(Cell, 0);
	}

	// Правило и стратегия - заново, как везде в проекте (см. Next()); геометрия
	// решётки и ChunkSize снимаются здесь, потому что промежуточные буферы
	// создаются уже в фоне, а живые UPROPERTY фоновому потоку читать нельзя.
	FCellularAutomatonRule AutomatonRule = BuildRule();
	TUniquePtr<FCellularAutomatonComputeStrategy> ComputeStrategy = CreateComputeStrategy();
	const FLatticeTransform LatticeSnapshot = BuildLatticeTransform();
	const int32 ChunkSizeSnapshot = ChunkSize;

	bStepInProgress = true;

	ShowStatusMessage(StatusKey_StepBackward,
		FString::Printf(TEXT("Шаг назад: пересчёт %lld поколений с нуля..."), TargetGeneration));

	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[AutomatonRule = MoveTemp(AutomatonRule), ComputeStrategy = MoveTemp(ComputeStrategy),
		 SeedGrid = MoveTemp(SeedGrid), WeakThis, TargetGeneration, LatticeSnapshot, ChunkSizeSnapshot]() mutable
		{
			const double StepStartSeconds = FPlatformTime::Seconds();

			// Тот же цикл, что в Next(), с одним отличием: источником владеет
			// сама лямбда (никакого сырого указателя на живой Grid - здесь его
			// и не нужно, пересчёт идёт от собственного засева), поэтому
			// предыдущее поколение освобождается сразу после того, как из него
			// посчитано следующее.
			TUniquePtr<FCellGrid> ResultGrid = MoveTemp(SeedGrid);
			int64 StepsDone = 0;
			while (StepsDone < TargetGeneration)
			{
				TUniquePtr<FCellGrid> NextGrid = MakeUnique<FDenseCellGrid>(LatticeSnapshot, ChunkSizeSnapshot, AutomatonRule.HasDecayStates());

				const int32 StepsRequested = static_cast<int32>(FMath::Min<int64>(TargetGeneration - StepsDone, MAX_int32));
				const int32 StepsAdvanced = ComputeStrategy->StepBatch(*ResultGrid, *NextGrid, AutomatonRule, StepsRequested);

				// Стратегия, продвинувшая больше одного поколения, обязана была
				// заполнить возрасты и угасание сама - см. Next().
				if (StepsAdvanced <= 1)
				{
					CellAging::ComputeAges(ResultGrid.Get(), *NextGrid);
					CellDecay::AdvanceDecayStates(ResultGrid.Get(), *NextGrid, AutomatonRule.GetStates());
				}

				ResultGrid = MoveTemp(NextGrid);
				StepsDone += FMath::Max(1, StepsAdvanced);
			}

			const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;
			const int64 ComputeUploadBytes = ComputeStrategy->GetLastComputeUploadBytes();

			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, ResultGrid = MoveTemp(ResultGrid), StepSeconds, TargetGeneration, ComputeUploadBytes]() mutable
			{
				AAutomataOrchestrator* StrongThis = WeakThis.Get();
				if (!StrongThis)
				{
					return;
				}

				StrongThis->Grid = MoveTemp(ResultGrid);
				StrongThis->SelectedCells.Reset();
				StrongThis->bStepInProgress = false;

				// R и N, нажатые пока шёл пересчёт, важнее его результата - оба
				// всё равно перестроят сетку с нуля (см. ApplyStepResult()).
				if (StrongThis->bResetToInitialStatePending)
				{
					StrongThis->bResetToInitialStatePending = false;
					StrongThis->ResetToInitialState();
					return;
				}

				if (StrongThis->bNewSeedPending)
				{
					StrongThis->bNewSeedPending = false;
					StrongThis->NewSeed();
					return;
				}

				// Счётчик выставляется, а не уменьшается: сетка теперь ровно то,
				// что даёт TargetGeneration шагов от изначального узора.
				StrongThis->GenerationCount = TargetGeneration;
				StrongThis->LastGpuComputeUploadBytes = ComputeUploadBytes;
				StrongThis->StepsSinceLastRender = 0;

				// График теряет только хвост после точки отката - история ДО неё
				// верна и переживает откат (см. GenerationHistory::TrimAfter()).
				GenerationHistory::TrimAfter(StrongThis->GenerationSamples, TargetGeneration);

				// Ещё один Ctrl+Z, нажатый пока считался этот - уходим в
				// следующий откат, не рисуя промежуточный кадр (он всё равно был
				// бы тут же заменён). Счётчик уже выставлен, так что новый
				// StepBackward() отсчитает от него.
				if (StrongThis->bStepBackwardPending)
				{
					StrongThis->bStepBackwardPending = false;
					StrongThis->StepBackward();
					return;
				}

				// Оболочка пересчитывается сразу, без своего интервала: она
				// описывает текущее поколение, а оно только что сменилось на
				// другое - причём назад, чего интервал не ожидает.
				if (StrongThis->bEnableGhostShape)
				{
					StrongThis->GhostShapeGenerationsSinceRefresh = 0;
					StrongThis->RefreshGhostShape();
				}

				// Немедленно и целиком, как ручной шаг: откат - осознанное
				// одиночное действие, размазывать его по кадрам незачем.
				StrongThis->RenderGridImmediate();

				StrongThis->ShowStatusMessage(StatusKey_StepBackward,
					FString::Printf(TEXT("Шаг назад: поколение %lld (пересчёт занял %.2f с)"),
						TargetGeneration, StepSeconds));

				UE_LOG(LogTemp, Log, TEXT("StepBackward: поколение %lld, живых клеток %d (пересчёт %lld поколений: %.2f мс [фоновый поток])"),
					TargetGeneration, StrongThis->Grid->Num(), TargetGeneration, StepSeconds * 1000.0);
			});
		});
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
	// Растяжение по Z - тоже из сетки и по той же причине. Берётся отношением
	// шагов, а не копированием UPROPERTY: в сетке лежит фактическая геометрия,
	// с которой клетки и расставлены.
	Header.LatticeZScale = Grid
		? static_cast<float>(Grid->GetLattice().GetCellWorldExtent().Z / FMath::Max(Grid->GetLattice().GetCellWorldExtent().X, UE_DOUBLE_SMALL_NUMBER))
		: LatticeZScale;
	// Фильтр чётности - часть геометрии не меньше, чем шаг: без него первое же
	// пересевание после загрузки (N/Y) уйдёт на другую подрешётку.
	Header.ParityFilter = GenerationParams.ParityFilter;
	Header.NeighborhoodShape = NeighborhoodShape;
	Header.ChunkSize = ChunkSize;
	Header.GridSize = GridSize;
	Header.Seed = Seed;
	// Amount/SpawnRadius пишутся из параметров ГЕНЕРАТОРА - отдельного блока
	// Automata|Random больше нет. Поля в заголовке оставлены как есть, чтобы
	// файлы читались и старой сборкой: для неё это по-прежнему радиус и число
	// клеток случайного шара, а смысл совпадает, когда выбран RandomBall.
	// ClusterFactor не пишется вовсе - он не влиял на генерацию уже давно
	// (GenerateRandom() передавал в генератор только радиус и количество), так
	// что в заголовке остаётся его значение по умолчанию.
	Header.Amount = GenerationParams.Amount;
	Header.SpawnRadius = GenerationParams.Radius;
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
	// Миграция: пока существовал отдельный радиус, нынешний VonNeumann2
	// записывался как VonNeumann с NeighborhoodRadius=2. Без этой строки такой
	// файл загрузился бы как VonNeumann - молча, с 6 соседями вместо 24 и
	// совсем другой картинкой. Поле в шапке оставлено только ради этой
	// проверки (см. FAutomatonSaveHeader::NeighborhoodRadius).
	if (Header.NeighborhoodRadius == 2 && Header.Neighborhood == ENeighborhood::VonNeumann)
	{
		Neighborhood = ENeighborhood::VonNeumann2;
	}
	States = FMath::Max(2, Header.States);
	CellSize = FMath::Max(1.0f, Header.CellSize);
	// Кламп повторяет ClampMin/UIMax самого UPROPERTY: шапка правится руками,
	// а нулевое или отрицательное растяжение схлопнуло бы решётку в плоскость.
	LatticeZScale = FMath::Clamp(Header.LatticeZScale, 0.1f, 10.0f);
	GenerationParams.ParityFilter = Header.ParityFilter;
	NeighborhoodShape = Header.NeighborhoodShape;
	ChunkSize = FMath::Max(1, Header.ChunkSize);
	GridSize.X = FMath::Max(1, Header.GridSize.X);
	GridSize.Y = FMath::Max(1, Header.GridSize.Y);
	GridSize.Z = FMath::Max(1, Header.GridSize.Z);
	Seed = Header.Seed;
	// Едут в параметры генератора. Тип при этом НЕ трогается: файл хранит
	// начальный набор клеток целиком (InitialCells), поэтому что именно им
	// когда-то построили, значения не имеет, а сбрасывать выбранный
	// пользователем генератор при загрузке было бы неожиданно.
	GenerationParams.Amount = FMath::Max(1, Header.Amount);
	GenerationParams.Radius = FMath::Max(1, Header.SpawnRadius);
	// Header.ClusterFactor намеренно игнорируется - см. BuildSaveHeader().
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

bool AAutomataOrchestrator::WriteStateToFile(const FString& FilePath, bool bUpdateLastSavePath)
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

	// Паспорт серии лежит в папке снимков и целью для следующего тихого Ctrl+S
	// быть не должен - см. doc-comment параметра.
	if (bUpdateLastSavePath)
	{
		LastSaveFilePath = FilePath;
	}

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

TUniquePtr<FCellGrid> AAutomataOrchestrator::CreateGrid() const
{
	return MakeUnique<FDenseCellGrid>(BuildLatticeTransform(), ChunkSize, States > 2);
}

FLatticeTransform AAutomataOrchestrator::BuildLatticeTransform() const
{
	return FLatticeTransform::MakeOrthogonal(CellSize, LatticeZScale);
}

FCellularAutomatonRule AAutomataOrchestrator::BuildRule() const
{
	// ЕДИНСТВЕННОЕ место, где решается, каким набором соседей считать. Это
	// не стилистика: правило строится в трёх местах (Next(), StepAsync() и
	// гистограмма Ctrl+Y), и если ветвление размножить, Ctrl+Y начнёт мерить
	// одно соседство, пока симуляция идёт по другому. Расхождение без всяких
	// симптомов, кроме "числа выглядят неправильно без причины".
	const TArray<FIntVector> LatticeOffsets = BuildLatticeNeighborOffsets(NeighborhoodShape);
	if (LatticeOffsets.Num() > 0)
	{
		return FCellularAutomatonRule(BirthCounts, SurvivalCounts, LatticeOffsets, States);
	}

	return FCellularAutomatonRule(BirthCounts, SurvivalCounts, Neighborhood, States);
}

TArray<FIntVector> AAutomataOrchestrator::BuildNeighborOffsetsForAnalysis() const
{
	// Ровно тот же выбор, что в BuildRule(), - гистограмма обязана мерить то
	// же соседство, по которому идёт симуляция.
	const TArray<FIntVector> LatticeOffsets = BuildLatticeNeighborOffsets(NeighborhoodShape);
	return LatticeOffsets.Num() > 0 ? LatticeOffsets : FCellularAutomatonRule::BuildNeighborOffsets(Neighborhood);
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
		StateGenerators::AnalyzeNeighborCounts(Cells, BuildNeighborOffsetsForAnalysis(), NeighborAnalysisSampleExtent, Histogram);
		HistogramText = StateGenerators::DescribeHistogram(Histogram);
	}

	RebuildGridFromCells(MoveTemp(Cells));

	UE_LOG(LogTemp, Log, TEXT("GenerateState: '%s' - клеток %d (перебрано %lld, генерация: %.2f мс)"),
		*GeneratorName, Grid->Num(), Stats.ScannedCells, Stats.Seconds * 1000.0);

	if (!HistogramText.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("GenerateState: соседи по %s - %s"),
			GetNeighborhoodDisplayName(Neighborhood),
			*HistogramText);
	}

	ShowStatusMessage(StatusKey_Generation,
		FString::Printf(TEXT("Генератор: %s - %d клеток"), *GeneratorName, Grid->Num()));
}

void AAutomataOrchestrator::AnalyzeLiveStructure()
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("AnalyzeLiveStructure: сетка ещё не создана"));
		return;
	}

	// Гвард на bStepInProgress намеренно НЕ ставится: фоновый шаг читает
	// *Grid, эта функция тоже только читает, а подменить Grid может лишь
	// ApplyStepResult() - то есть игровой поток, тот же, что выполняет эту
	// функцию. Двух одновременных читателей const-структуры достаточно.
	TArray<FIntVector> AliveCells;
	Grid->GetAliveCells(AliveCells);

	if (AliveCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("AnalyzeLiveStructure: живых клеток нет"));
		ShowStatusMessage(StatusKey_Generation, TEXT("Гистограмма: живых клеток нет"));
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();

	StateGenerators::FNeighborHistogram Histogram;
	StateGenerators::AnalyzeNeighborCounts(AliveCells, BuildNeighborOffsetsForAnalysis(), LiveAnalysisSampleExtent, Histogram);

	const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

	// Доля выборки печатается всегда: на эволюционировавшей структуре
	// центральный подкуб может оказаться и всей структурой, и одним процентом
	// от неё, а по самой гистограмме этого не видно.
	const double SampleShare = AliveCells.Num() > 0
		? 100.0 * double(Histogram.SampledAlive) / double(AliveCells.Num())
		: 0.0;

	// Правило строится из тех же Details-panel свойств, по которым идёт
	// симуляция (та же конвенция "пересобирать каждый вызов, ничего не
	// кэшировать", что в Next()/StepAsync()) - иначе сводка ниже могла бы
	// описывать не то правило, которое реально считает.
	const FCellularAutomatonRule Rule(BirthCounts, SurvivalCounts, Neighborhood, States);

	// Сводка - ради неё вся функция и нужна: гистограмма отвечает на вопрос
	// "какое распределение", а это - на вопрос "куда по нему бьют пороги".
	int64 Doomed = 0;
	for (int32 Count = 0; Count < Histogram.AliveByCount.Num(); ++Count)
	{
		if (!Rule.GetSurvivalCounts().Contains(Count))
		{
			Doomed += Histogram.AliveByCount[Count];
		}
	}

	int64 Births = 0;
	for (int32 Count = 0; Count < Histogram.EmptyByCount.Num(); ++Count)
	{
		if (Rule.GetBirthCounts().Contains(Count))
		{
			Births += Histogram.EmptyByCount[Count];
		}
	}

	const int64 Survivors = Histogram.SampledAlive - Doomed;
	const double DoomedShare = Histogram.SampledAlive > 0
		? 100.0 * double(Doomed) / double(Histogram.SampledAlive)
		: 0.0;
	// Оценка, а не точное число: рождения считаются по пустым клеткам,
	// примыкающим к выборке, а часть их лежит уже ЗА границей подкуба, тогда
	// как знаменатель - строго клетки выборки. На однородной структуре
	// отношение всё равно показывает направление и порядок величины.
	const double GrowthFactor = Histogram.SampledAlive > 0
		? double(Survivors + Births) / double(Histogram.SampledAlive)
		: 0.0;

	// При Generations клетка, переставшая выживать, не умирает, а уходит в
	// угасание - слово должно быть другим. Строка собирается заранее:
	// format-строка у Printf проверяется на этапе компиляции и обязана быть
	// литералом, тернарник прямо в вызове не соберётся.
	FString DoomedWord = TEXT("умрут");
	FString DecayNote;
	if (Rule.HasDecayStates())
	{
		DoomedWord = TEXT("уйдут в угасание");
		// Угасающие клетки не живые, поэтому в гистограмме они попадают в
		// "примыкающие пустые" - а родиться там нельзя (birth-immunity), так
		// что оценка рождений при Generations завышена.
		DecayNote = TEXT(" (Generations: рождения завышены - часть 'пустых' на деле угасающие и рождению не подлежат)");
	}

	UE_LOG(LogTemp, Log, TEXT("AnalyzeLiveStructure: правило %s, соседство %s (%d соседей), поколение %d"),
		*GetActiveRuleString(), GetNeighborhoodDisplayName(Neighborhood),
		Rule.GetNeighborOffsets().Num(), GenerationCount);
	UE_LOG(LogTemp, Log, TEXT("AnalyzeLiveStructure: живых всего %d, в выборке %lld (%.1f%%, подкуб +-%d), посчитано за %.2f мс"),
		AliveCells.Num(), Histogram.SampledAlive, SampleShare, LiveAnalysisSampleExtent, ElapsedSeconds * 1000.0);
	UE_LOG(LogTemp, Log, TEXT("AnalyzeLiveStructure: %s"), *StateGenerators::DescribeHistogram(Histogram));
	UE_LOG(LogTemp, Log, TEXT("AnalyzeLiveStructure: ИТОГО %s %lld (%.1f%%), выживут %lld, родятся %lld -> нетто %+lld, x%.2f%s"),
		*DoomedWord, Doomed, DoomedShare, Survivors, Births, Births - Doomed, GrowthFactor, *DecayNote);

	ShowStatusMessage(StatusKey_Generation,
		FString::Printf(TEXT("Соседи: %s %lld из %lld (%.0f%%), родятся %lld, x%.2f - подробности в логе"),
			*DoomedWord, Doomed, Histogram.SampledAlive, DoomedShare, Births, GrowthFactor));
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
	RasterParams.CellWorldStep = Grid->GetLattice().GetCellWorldExtent();
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

	// Отражение - постобработка над готовым снимком, а не отдельный путь
	// растеризации: тайл это то же изображение плюс его зеркала, и знать про
	// клетки для этого не нужно.
	if (SliceCaptureParams.TileMode != ESliceTileMode::None)
	{
		const bool bMirrorX = (SliceCaptureParams.TileMode == ESliceTileMode::MirrorX || SliceCaptureParams.TileMode == ESliceTileMode::MirrorBoth);
		const bool bMirrorY = (SliceCaptureParams.TileMode == ESliceTileMode::MirrorY || SliceCaptureParams.TileMode == ESliceTileMode::MirrorBoth);
		CellRasterizer::MakeTile(Image, bMirrorX, bMirrorY);
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
	RasterParams.CellWorldStep = Grid->GetLattice().GetCellWorldExtent();
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

TArray<FCapturePreset> AAutomataOrchestrator::GetCapturePresets() const
{
	return CapturePresets::GetAll();
}

FString AAutomataOrchestrator::GetActiveCapturePresetName() const
{
	const TArray<FCapturePreset>& Presets = CapturePresets::GetAll();
	return Presets.IsValidIndex(ActiveCapturePresetIndex) ? Presets[ActiveCapturePresetIndex].Name : FString();
}

void AAutomataOrchestrator::ApplyCapturePreset(int32 PresetIndex)
{
	const TArray<FCapturePreset>& Presets = CapturePresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyCapturePreset: индекс %d вне диапазона (наборов: %d)"), PresetIndex, Presets.Num());
		return;
	}

	const FCapturePreset& Preset = Presets[PresetIndex];

	// Присваивание целиком, а не поле за полем: в этом и смысл того, что пресет
	// хранит всю структуру - хвостов от предыдущего набора не остаётся по
	// построению, а не потому, что кто-то не забыл их сбросить.
	SliceCaptureParams = Preset.Params;
	ActiveCapturePresetIndex = PresetIndex;

	// Идущую серию НЕ трогаем: её длина и шаг зафиксированы на старте
	// (SeriesFramesRemaining), а размер кадра поменялся бы прямо посреди неё -
	// получилась бы серия из кадров разного размера, непригодная ни для
	// анимации, ни для перебора.
	if (bSeriesCaptureActive)
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyCapturePreset: серия ещё идёт - новый набор вступит в силу со следующего запуска F7"));
	}

	UE_LOG(LogTemp, Log, TEXT("ApplyCapturePreset: '%s' (режим %d, плитка %d, %d пикс/клетка, серия %d x %d поколений)"),
		*Preset.Name,
		static_cast<int32>(SliceCaptureParams.Mode),
		static_cast<int32>(SliceCaptureParams.TileMode),
		SliceCaptureParams.PixelsPerCell,
		SliceCaptureParams.SeriesFrameCount,
		SliceCaptureParams.SeriesGenerationsPerFrame);

	ShowStatusMessage(StatusKey_SliceCapture,
		FString::Printf(TEXT("Съёмка: %s - %s"), *Preset.Name, *Preset.Description));
}

void AAutomataOrchestrator::CycleCapturePreset()
{
	const TArray<FCapturePreset>& Presets = CapturePresets::GetAll();
	if (Presets.Num() == 0)
	{
		return;
	}

	// INDEX_NONE (ни одного набора ещё не применяли) даёт 0 - первый набор, а не
	// второй: настройки правили руками, и логичный первый шаг перебора - начало
	// списка.
	const int32 NextIndex = Presets.IsValidIndex(ActiveCapturePresetIndex)
		? (ActiveCapturePresetIndex + 1) % Presets.Num()
		: 0;

	ApplyCapturePreset(NextIndex);
}

void AAutomataOrchestrator::WriteSeriesManifest()
{
	if (SeriesDirectory.IsEmpty())
	{
		return;
	}

	const int32 PerFrame = FMath::Max(SliceCaptureParams.SeriesGenerationsPerFrame, 1);
	// Поколение, на котором серия начинается. Кадр N снят на
	// StartGeneration + N * PerFrame - это и есть та единственная координата,
	// которой не хватает в самом PNG.
	const int64 StartGeneration = GenerationCount;

	// .casave - тем же путём, что Ctrl+S, вплоть до миниатюры: отдельный
	// формат "почти как сохранение" разъехался бы с настоящим при первой же
	// правке шапки. LastSaveFilePath при этом не трогаем.
	const FString SavePath = SeriesDirectory / TEXT("Series.casave");
	const bool bSaved = WriteStateToFile(SavePath, /*bUpdateLastSavePath=*/false);
	if (!bSaved)
	{
		// Причина уже в логе. Серию не прерываем - кадры важнее паспорта.
		UE_LOG(LogTemp, Warning, TEXT("WriteSeriesManifest: состояние не сохранено, кадры серии останутся без точки возврата"));
	}

	// Текстовая записка рядом с бинарником: .casave не прочитать в Проводнике,
	// а понять, что за папка, нужно бывает без запуска редактора вовсе.
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Серия снимков клеточного автомата")));
	Lines.Add(FString::Printf(TEXT("Снято: %s"), *FDateTime::Now().ToString()));
	Lines.Add(TEXT(""));
	Lines.Add(FString::Printf(TEXT("Правило:              %s"), *GetActiveRuleString()));
	Lines.Add(FString::Printf(TEXT("Seed:                 %d"), Seed));
	Lines.Add(FString::Printf(TEXT("Генератор: %s, Radius / Amount: %d / %d"),
		*StateGenerators::GetDisplayName(GenerationParams.Type), GenerationParams.Radius, GenerationParams.Amount));
	Lines.Add(FString::Printf(TEXT("CellSize / ChunkSize: %.1f / %d"), CellSize, ChunkSize));
	Lines.Add(TEXT(""));
	Lines.Add(FString::Printf(TEXT("Поколение на старте:  %lld"), StartGeneration));
	Lines.Add(FString::Printf(TEXT("Поколений на кадр:    %d"), PerFrame));
	Lines.Add(FString::Printf(TEXT("Кадров запрошено:     %d"), SeriesFramesRemaining));
	Lines.Add(TEXT(""));
	Lines.Add(FString::Printf(TEXT("Набор съёмки:         %s"), *GetActiveCapturePresetName()));
	Lines.Add(FString::Printf(TEXT("Режим растеризации:   %d (0=NearestToCamera, 1=Silhouette, 2=Thickness)"),
		static_cast<int32>(SliceCaptureParams.Mode)));
	Lines.Add(FString::Printf(TEXT("Плитка:               %d (0=None, 1=MirrorX, 2=MirrorY, 3=MirrorBoth)"),
		static_cast<int32>(SliceCaptureParams.TileMode)));
	Lines.Add(FString::Printf(TEXT("Пикселей на клетку:   %d"), SliceCaptureParams.PixelsPerCell));
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("Как переснять кадр Frame_NNNN крупнее или в другом цвете:"));
	Lines.Add(TEXT("  1. Ctrl+O -> Series.casave из этой папки."));
	Lines.Add(FString::Printf(TEXT("  2. Выставить StepsPerRender = %d (клавиши T/G)."), PerFrame));
	Lines.Add(TEXT("  3. Нажать F ровно NNNN раз (счётчик поколений в HUD - ориентир)."));
	Lines.Add(TEXT("  4. Shift+F7 до нужного набора, затем F6."));
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("Цвет пересъёмке не мешает: снимок растеризуется из сетки, поэтому"));
	Lines.Add(TEXT("то же правило + тот же сид + то же поколение дают тот же кадр"));
	Lines.Add(TEXT("бит в бит при любой палитре AgeColors."));

	const FString ManifestPath = SeriesDirectory / TEXT("Series.txt");
	// ForceUTF8 - это UTF-8 ИМЕННО с BOM (без него - отдельное значение
	// ForceUTF8WithoutBOM). BOM здесь обязателен: без него Блокнот прочитает
	// кириллицу как мусор.
	if (!FFileHelper::SaveStringArrayToFile(Lines, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8))
	{
		UE_LOG(LogTemp, Warning, TEXT("WriteSeriesManifest: не удалось записать %s"), *ManifestPath);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("WriteSeriesManifest: паспорт серии записан (%s), поколение на старте %lld, %d поколений на кадр"),
		bSaved ? TEXT("Series.casave + Series.txt") : TEXT("только Series.txt"), StartGeneration, PerFrame);
}

void AAutomataOrchestrator::StartSeriesCapture()
{
	if (bSeriesCaptureActive)
	{
		// Повторный вызов - обрыв: у хоткея одна клавиша на оба действия, и
		// прервать серию должно быть так же просто, как начать.
		StopSeriesCapture();
		return;
	}

	if (!Grid || Grid->Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartSeriesCapture: сетка пуста - снимать нечего"));
		ShowStatusMessage(StatusKey_SliceCapture, TEXT("Серия не начата: сетка пуста"));
		return;
	}

	// Своя подпапка на запуск: кадры одной серии должны лежать вместе и
	// нумероваться подряд, иначе их не собрать в анимацию.
	SeriesDirectory = EnsureSliceDirectory() / FString::Printf(TEXT("Series_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	IFileManager::Get().MakeDirectory(*SeriesDirectory, /*Tree=*/true);

	SeriesFrameIndex = 0;
	SeriesFramesRemaining = FMath::Max(SliceCaptureParams.SeriesFrameCount, 1);
	SeriesGenerationsSinceFrame = 0;
	bSeriesCaptureActive = true;

	UE_LOG(LogTemp, Log, TEXT("StartSeriesCapture: %d кадров через каждые %d поколений -> %s"),
		SeriesFramesRemaining, FMath::Max(SliceCaptureParams.SeriesGenerationsPerFrame, 1), *SeriesDirectory);

	// Паспорт серии пишется ДО первого кадра: даже оборванная на первом же
	// снимке серия оставляет папку, по которой понятно, что это было.
	WriteSeriesManifest();

	// Текущее состояние - это тоже кадр серии, и притом единственный, который
	// пользователь видел глазами, когда решил снимать.
	CaptureSeriesFrame();

	// Серия могла закончиться на первом же кадре (SeriesFrameCount == 1).
	if (!bSeriesCaptureActive)
	{
		return;
	}

	if (!bSimulationRunning && !IsFastStepActive())
	{
		bSeriesStartedSimulation = true;
		Start();
	}
}

void AAutomataOrchestrator::StopSeriesCapture()
{
	if (!bSeriesCaptureActive)
	{
		return;
	}

	const int32 Captured = SeriesFrameIndex;
	bSeriesCaptureActive = false;
	SeriesFramesRemaining = 0;
	SeriesGenerationsSinceFrame = 0;

	// Останавливаем только то, что сами и запустили.
	if (bSeriesStartedSimulation)
	{
		bSeriesStartedSimulation = false;
		if (bSimulationRunning)
		{
			Stop();
		}
	}

	// Догоняющего рендера здесь нет намеренно. В быстром режиме промежуточные
	// поколения на экран не выводились, и на нём осталась картинка того
	// состояния, с которого серию запустили (F7) - именно она и должна
	// остаться. Финальное поколение уже лежит в последнем PNG, а рендер
	// многомиллионной сетки - самая дорогая операция кадра, и платить за неё
	// ради картинки, которую всё равно смотрят в файлах, незачем. Экран
	// обновится сам при следующем рендере (P, F, R, N).

	UE_LOG(LogTemp, Log, TEXT("StopSeriesCapture: снято %d кадров -> %s"), Captured, *SeriesDirectory);
	ShowStatusMessage(StatusKey_SliceCapture,
		FString::Printf(TEXT("Серия завершена: %d кадров в %s"), Captured, *FPaths::GetCleanFilename(SeriesDirectory)));
}

void AAutomataOrchestrator::CaptureSeriesFrame()
{
	const FString FileName = FString::Printf(TEXT("Frame_%04d.png"), SeriesFrameIndex);

	if (!WriteSliceCaptureToFile(SeriesDirectory / FileName))
	{
		// Причину уже сказал WriteSliceCaptureToFile(). Серию обрываем: если
		// кадр не снялся, следующие почти наверняка не снимутся тоже, а
		// молча продолжать капать ошибками в лог - худший вариант.
		UE_LOG(LogTemp, Warning, TEXT("CaptureSeriesFrame: кадр %d не снят - серия прервана"), SeriesFrameIndex);
		StopSeriesCapture();
		return;
	}

	++SeriesFrameIndex;
	--SeriesFramesRemaining;

	if (SeriesFramesRemaining <= 0)
	{
		StopSeriesCapture();
		return;
	}

	// Своё сообщение поверх того, что показал WriteSliceCaptureToFile(): в
	// серии важнее прогресс, чем размер очередного файла.
	ShowStatusMessage(StatusKey_SliceCapture,
		FString::Printf(TEXT("Серия: кадр %d из %d"), SeriesFrameIndex, SeriesFrameIndex + SeriesFramesRemaining));
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
		UE_LOG(LogTemp, Warning, TEXT("Next: сетка не инициализирована - сначала постройте состояние (хоткей Y / GenerateState)"));
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
	FCellularAutomatonRule AutomatonRule = BuildRule();
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
	// геометрию решётки и ChunkSize (UPROPERTY, могут править в Details panel)
	// снимаем здесь - фоновый поток не должен их читать.
	const FLatticeTransform LatticeSnapshot = BuildLatticeTransform();
	const int32 ChunkSizeSnapshot = ChunkSize;

	bStepInProgress = true;

	FCellGrid* CurrentGridPtr = Grid.Get();
	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	// Сырой указатель на *Grid без защиты времени жизни - как и в StepAsync(),
	// EndPlay() дожидается PendingStepFuture перед разрушением актора, а все
	// остальные пути замены Grid отказываются работать при bStepInProgress.
	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[AutomatonRule = MoveTemp(AutomatonRule), ComputeStrategy = MoveTemp(ComputeStrategy),
		 CurrentGridPtr, WeakThis, NumSteps, LatticeSnapshot, ChunkSizeSnapshot]() mutable
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
				TUniquePtr<FCellGrid> NextGrid = MakeUnique<FDenseCellGrid>(LatticeSnapshot, ChunkSizeSnapshot, AutomatonRule.HasDecayStates());
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

				// Отложенный шаг назад (Ctrl+Z) - до увеличения GenerationCount
				// ниже, по той же причине, что и в ApplyStepResult().
				if (StrongThis->bStepBackwardPending)
				{
					StrongThis->bStepBackwardPending = false;
					StrongThis->StepBackward();
					return;
				}

				// Вымирание ловится и на ручном шаге - см.
				// bAutoReseedOnExtinction и ту же проверку в ApplyStepResult().
				if (StrongThis->TryAutoReseedOnExtinction(NumSteps))
				{
					return;
				}

				// NumSteps реально посчитанных поколений за одно нажатие F -
				// см. GenerationCount/FHudStats.
				StrongThis->GenerationCount += NumSteps;
				StrongThis->LastGpuComputeUploadBytes = ComputeUploadBytes;

				// Точка графика. Ручной шаг всегда рисует (ниже), так что
				// перенесённое сюда значение "видимо" тут же исправится на
				// фактическое - но появиться замер обязан здесь, рядом со
				// счётчиком, а не в рендере: так одно и то же место отвечает
				// за "поколение состоялось" в обеих ветках, ручной и Play.
				StrongThis->AppendGenerationSample();

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
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: сетка не инициализирована - сначала постройте состояние (хоткей Y / GenerateState)"));
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
	FCellularAutomatonRule AutomatonRule = BuildRule();
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
	// создаются уже в фоне, поэтому геометрия решётки и ChunkSize - живые
	// UPROPERTY, которые фоновому потоку трогать нельзя - снимаем здесь. Тот
	// же приём, что в Next().
	const FLatticeTransform LatticeSnapshot = BuildLatticeTransform();
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
		 BatchGenerations, LatticeSnapshot, ChunkSizeSnapshot]() mutable
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
				NextGridBuffer = MakeUnique<FDenseCellGrid>(LatticeSnapshot, ChunkSizeSnapshot, AutomatonRule.HasDecayStates());
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

	// Видимость куба на отсечение НЕ влияет - см. doc-comment в заголовке.
	return EnsureRenderCullVolume();
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
	CellsRenderer->SetMaterial(EnsureCellMaterialInstance());
	// Задаётся явно на каждый рендер: тот же класс рендерера используется и для
	// подсветки выделения, где множитель свой (см. SelectionScaleMultiplier).
	// Инвариант "обычные клетки берут CellMeshScaleMultiplier" лучше держать
	// локально и видимо, чем полагаться на то, что этих двух рендереров никто
	// никогда не смешает.
	CellsRenderer->SetScaleMultiplier(CellMeshScaleMultiplier);

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
		// Ноль - правда, а не отсутствие данных: детальных инстансов в
		// AddInstances() ушло ровно столько. Провал линии "видимо" в ноль при
		// включении Ghost Shape и есть та диагностика, ради которой график
		// делается.
		NoteRenderedCells(0);
		RenderSelectionOverlay();
		UE_LOG(LogTemp, Log, TEXT("RenderGridImmediate: детальный рендер пропущен - Ghost Shape покрывает всю сетку целиком (%d живых клеток)"),
			Grid->Num());
		return;
	}

	TArray<FCellRenderInstance> Instances;
	BuildCellRenderData(Instances);
	// До Render() ниже: там Instances уже перемещён.
	NoteRenderedCells(LastRenderStats.RenderedCellCount);

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
	CellsRenderer->SetMaterial(EnsureCellMaterialInstance());
	// См. одноимённый комментарий в RenderGridImmediate().
	CellsRenderer->SetScaleMultiplier(CellMeshScaleMultiplier);

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
		// См. ту же ветку в RenderGridImmediate() - ноль здесь фактический.
		NoteRenderedCells(0);
		RenderSelectionOverlay();
		UE_LOG(LogTemp, Log, TEXT("RenderCurrentGrid: детальный рендер пропущен - Ghost Shape покрывает всю сетку целиком (%d живых клеток)"),
			Grid->Num());
		return;
	}

	TArray<FCellRenderInstance> Instances;
	BuildCellRenderData(Instances);
	// До BeginRender()/Render() ниже: там Instances уже перемещён. Значение -
	// это то, что уйдёт в AddInstances() целиком, даже если чанковый рендер
	// размажет его по кадрам: график про объём работы, а не про текущий кадр.
	NoteRenderedCells(LastRenderStats.RenderedCellCount);

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

	// То же для Ctrl+Z (см. doc-comment bStepBackwardPending). Стоит ДО
	// увеличения GenerationCount ниже, и это принципиально: StepBackward()
	// отсчитывает от него, а поколение, только что посчитанное этим самым
	// заходом, на экране ещё не было. Учтя его, откат вернул бы ровно то, что
	// сейчас в Grid, и нажатие не изменило бы ничего видимого.
	if (bStepBackwardPending)
	{
		bStepBackwardPending = false;
		StepBackward();
		return;
	}

	// Сетка вымерла, а режим брутфорса включён - катим следующий сид вместо
	// того, чтобы рисовать пустоту (см. bAutoReseedOnExtinction). Проверка
	// стоит ПЕРЕД счётчиками и рендером ниже, потому что NewSeed() всё равно
	// перестроит сетку с нуля и обнулит их (RebuildGridFromCells()).
	if (TryAutoReseedOnExtinction(Generations))
	{
		return;
	}

	// Реально посчитанные поколения - считаем для HUD независимо от того,
	// пропустит ли StepsSinceLastRender ниже фактический рендер (см.
	// GenerationCount/FHudStats).
	GenerationCount += Generations;

	// Точка графика - здесь же, ДО обеих проверок пропуска рендера ниже, по
	// той же причине, по которой тут стоят серийная съёмка и Ghost Shape:
	// линия "всего клеток" описывает симуляцию, а не экран, и обязана
	// существовать для поколений, до AddInstances() не дошедших. Значение
	// "видимо" переносится с прошлого замера и исправляется на фактическое
	// в RenderCurrentGrid() ниже, если это поколение всё-таки рисуется.
	AppendGenerationSample();

	// Серия снимков идёт по своему счётчику ПОКОЛЕНИЙ - как и Ghost Shape
	// ниже, и по той же причине: шагом заходов было бы неравномерно (один
	// заход может посчитать сразу пачку), а шагом кадров экрана - зависело бы
	// от скорости отрисовки. Съёмка не смотрит на StepsSinceLastRender: она
	// растеризует сетку сама и не нуждается в том, чтобы поколение попало на
	// экран.
	//
	// Решение "рисовать ли это поколение" снимается ЗДЕСЬ, до съёмки, а не в
	// самой проверке ниже: последний кадр серии заканчивается вызовом
	// StopSeriesCapture() прямо из CaptureSeriesFrame(), и тот сбрасывает
	// bSeriesCaptureActive. Прочитанный после этого флаг сказал бы "серии нет",
	// и финальное поколение - единственное из всех - уехало бы в AddInstances,
	// хотя оно уже лежит в последнем PNG.
	const bool bSeriesSkipsRender = bSeriesCaptureActive && SliceCaptureParams.bSeriesFastMode;
	if (bSeriesCaptureActive)
	{
		SeriesGenerationsSinceFrame += Generations;
		if (SeriesGenerationsSinceFrame >= FMath::Max(1, SliceCaptureParams.SeriesGenerationsPerFrame))
		{
			SeriesGenerationsSinceFrame = 0;
			CaptureSeriesFrame();
		}
	}

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

	// Серия в быстром режиме не рисует промежуточные поколения вовсе: снимок
	// растеризуется прямо из сетки, и поколению незачем попадать на экран,
	// чтобы попасть в файл, а рендер клеток - самая дорогая часть кадра.
	// Экран так и остаётся на состоянии, с которого серию запустили, - в том
	// числе после её окончания (см. StopSeriesCapture() и bSeriesSkipsRender
	// выше: флаг снят до съёмки, поэтому последнее поколение серии тоже сюда
	// не проходит).
	if (bSeriesSkipsRender)
	{
		return;
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

const FName AAutomataOrchestrator::CellBorderWidthParameter(TEXT("BorderWidth"));

void AAutomataOrchestrator::SetCellBorderWidth(float NewBorderWidth)
{
	// Тот же зажим, что в meta у самого свойства: сеттер существует ради
	// слайдера HUD, а тот пишет значение напрямую и об ограничениях панели не
	// знает.
	CellBorderWidth = FMath::Clamp(NewBorderWidth, 0.0f, 0.25f);

	// Ни перерисовки, ни пересчёта поколения: значение уезжает в uniform-буфер
	// материала и видно уже на следующем кадре, даже на полностью
	// остановленной симуляции.
	EnsureCellMaterialInstance();
}

UMaterialInterface* AAutomataOrchestrator::EnsureCellMaterialInstance()
{
	if (!CellMaterial)
	{
		CellMaterialInstance = nullptr;
		return nullptr;
	}

	// Parent, а не GetBaseMaterial(): последний возвращает корневой UMaterial,
	// поэтому подмена CellMaterial на другой Material Instance того же родителя
	// осталась бы незамеченной, и клетки продолжили бы рисоваться прежним.
	if (!CellMaterialInstance || CellMaterialInstance->Parent != CellMaterial)
	{
		CellMaterialInstance = UMaterialInstanceDynamic::Create(CellMaterial, this);
		bCellBorderParameterWarned = false;
	}

	if (!CellMaterialInstance)
	{
		// Рисовать без канта лучше, чем не рисовать вовсе.
		UE_LOG(LogTemp, Warning, TEXT("EnsureCellMaterialInstance: не удалось создать динамический инстанс материала - ширина контура меняться не будет"));
		return CellMaterial;
	}

	// Отсутствующий параметр - тихий отказ: SetScalarParameterValue() в этом
	// случае просто ничего не делает, и снаружи это выглядит как сломанный
	// ползунок. Проверяем один раз на инстанс и говорим об этом вслух.
	if (!bCellBorderParameterWarned)
	{
		float ExistingValue = 0.0f;
		if (!CellMaterialInstance->GetScalarParameterValue(FMaterialParameterInfo(CellBorderWidthParameter), ExistingValue))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("EnsureCellMaterialInstance: в материале клеток нет скалярного параметра '%s' - CellBorderWidth ни на что не влияет"),
				*CellBorderWidthParameter.ToString());
		}
		bCellBorderParameterWarned = true;
	}

	CellMaterialInstance->SetScalarParameterValue(CellBorderWidthParameter, CellBorderWidth);
	return CellMaterialInstance;
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

void AAutomataOrchestrator::TakePhotoShot()
{
	// 0. Проверяем размер ДО любых побочных эффектов - тот же принцип, что у
	// бюджетов бейка и генератора: отказ обязан оставить всё как было, а не
	// остановить симуляцию, снять отсечения и переключить профиль ради снимка,
	// которого не будет.
	int32 Width = 0;
	int32 Height = 0;
	if (!ValidatePhotoShotResolution(Width, Height))
	{
		return;
	}

	// 1. Кадр обязан быть неподвижен. HighResShot снимает тайлами, по одному
	// проходу на тайл, и между проходами проходят кадры игры - шагающая
	// симуляция склеилась бы из разных поколений.
	if (bSimulationRunning)
	{
		Stop();
	}
	if (bFastStepActive)
	{
		StopFastStep();
	}

	// 2. Всё, что настроено для РАЗГЛЯДЫВАНИЯ, остаётся как есть: куб отсечения
	// (C), срез вдоль взгляда (J), фильтр по возрасту (цифры) и спрятанный фон
	// (U). Снимают ровно то, что видят, - если человек вырезал кубом кусок
	// структуры и убрал небо, значит именно это и хочет получить картинкой.
	//
	// Первая версия гасила срез и фильтр по возрасту "чтобы в кадр попало всё",
	// и это было ошибкой: она молча выбрасывала настройку, ради которой кадр и
	// выстраивали. Куб и так не трогался - профили рендера им не владеют.
	//
	// Профиль ниже владеет только КАЧЕСТВОМ (свет, тени, Lumen, сглаживание) и
	// двумя ускорителями, которые портят снимок, - отсечением по расстоянию и
	// Ghost Shape. Фон в профиле тоже есть, поэтому его выбор приходится
	// восстанавливать вручную: профиль обязан задавать все свои поля целиком
	// (см. doc-comment FRenderPreset), исключений в таблице не предусмотрено.
	const bool bBackgroundWasVisible = bShowBackground;

	// 3. Профиль съёмки - обычный или экономный по памяти. Своей F-клавиши ни у
	// одного из них нет, оба применяются только отсюда.
	const int32 PhotoPresetIndex = RenderPresets::GetPhotoPresetIndex(bPhotoLeanMemory);
	if (PhotoPresetIndex == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePhotoShot: профиль съёмки не найден в таблице - снимок отменён"));
		return;
	}
	ApplyRenderPreset(PhotoPresetIndex);

	// 3.5. Вернуть выбор по фону. Через сеттер, а не записью поля: он и применит
	// видимость к компонентам, и поднимет bRenderPresetModified - профиль после
	// этого действительно описывает экран не полностью, и HUD обязан показать
	// это звёздочкой, а не врать именем профиля.
	if (bShowBackground != bBackgroundWasVisible)
	{
		SetBackgroundVisible(bBackgroundWasVisible);
	}

	// 3.7. Убрать из кадра инструменты редактирования - коробку куба, её ручки и
	// подсветку выделения. До рендера ниже, чтобы перерисовка сразу учла это.
	HideEditingVisualsForPhoto();

	// 4. Перерисовать целиком и немедленно: ApplyRenderPreset() уже
	// перерисовывает через сеттер Ghost Shape, но полагаться на это нельзя -
	// он перерисует только если флаг силуэта реально сменился.
	const double RenderStartSeconds = FPlatformTime::Seconds();
	RenderGridImmediate();
	const double RenderSeconds = FPlatformTime::Seconds() - RenderStartSeconds;

	// 5. Сколько раз отрисовать кадр перед сохранением, и сам снимок. Это не
	// пауза, а множитель времени съёмки - см. doc-comment PhotoShotDelayFrames.
	// Ставим каждый раз, а не один раз при старте: свойство редактируется в
	// Details, и значение должно означать себя на КАЖДОМ снимке, а не на первом.
	const int32 DelayFrames = FMath::Max(1, PhotoShotDelayFrames);
	RunRenderConsoleCommand(FString::Printf(TEXT("r.HighResScreenshotDelay %d"), DelayFrames));

	const int32 CellCount = Grid.IsValid() ? Grid->Num() : 0;
	const double Megapixels = (double(Width) * double(Height)) / 1000000.0;

	// Память под текстуры на момент съёмки - чтобы "жрёт немерено" стало
	// числом. Мерить надо ДО выстрела: сам снимок аллоцирует свои буферы уже
	// внутри рендер-потока, и сравнивать имеет смысл базу "экономный профиль
	// против обычного", а не пик.
	FTextureMemoryStats MemoryStats;
	RHIGetTextureMemoryStats(MemoryStats);
	const double AllocatedMB = double(MemoryStats.StreamingMemorySize + MemoryStats.NonStreamingMemorySize) / (1024.0 * 1024.0);
	const double TotalMB = MemoryStats.TotalGraphicsMemory > 0 ? double(MemoryStats.TotalGraphicsMemory) / (1024.0 * 1024.0) : 0.0;

	UE_LOG(LogTemp, Log, TEXT("=== Снимок: %dx%d (%.1f Мпикс) x%d прогонов | профиль %s | живых клеток %d ==="),
		Width, Height, Megapixels, DelayFrames, bPhotoLeanMemory ? TEXT("Photo Lean") : TEXT("Photo"), CellCount);
	UE_LOG(LogTemp, Log, TEXT("Снимок: подготовка кадра заняла %.2f с; текстур занято %.0f МБ из %.0f МБ"),
		RenderSeconds, AllocatedMB, TotalMB);
	UE_LOG(LogTemp, Log, TEXT("Снимок: команда выдана, дальше движок нарисует кадр %d раз подряд в разрешении снимка. Окно замрёт до конца, промежуточных сообщений не будет - следующая строка появится уже по готовности файла."),
		DelayFrames);

	ShowStatusMessage(StatusKey_PhotoShot, FString::Printf(TEXT("[F10] Снимок %dx%d (%.1f Мпикс) x%d прогонов - окно замрёт до конца"),
		Width, Height, Megapixels, DelayFrames));

	// Метка для замера длительности - см. PhotoShotIssuedSeconds. Ставится
	// ПЕРЕД выдачей команды, хотя команда и возвращается мгновенно: сам кадр
	// рисуется позже, и отсчёт должен начинаться отсюда.
	PhotoShotIssuedSeconds = FPlatformTime::Seconds();
	PhotoShotIssuedFrame = GFrameCounter;
	// Тик мог быть выключён - Stop() выше его гасит, а без тика некому будет
	// заметить, что съёмка кончилась, и напечатать итог.
	SetActorTickEnabled(true);

	RunRenderConsoleCommand(FString::Printf(TEXT("HighResShot %dx%d"), Width, Height));
}

void AAutomataOrchestrator::HideEditingVisualsForPhoto()
{
	bPhotoRestoreVolumeVisible = false;
	bPhotoRestoreGizmoVisible = false;
	bPhotoRestoreSelectionVisible = false;

	// EnsureRenderCullVolume(), а не GetActiveCullVolume(): коробку надо
	// спрятать независимо от того, режет она сейчас или нет - в кадре она
	// мешает в любом случае.
	if (ARenderCullVolume* CullVolume = EnsureRenderCullVolume())
	{
		bPhotoRestoreVolumeVisible = CullVolume->IsVolumeVisible();
		bPhotoRestoreGizmoVisible = CullVolume->IsGizmoVisible();

		if (bPhotoRestoreGizmoVisible)
		{
			CullVolume->SetGizmoVisible(false);
		}
		if (bPhotoRestoreVolumeVisible)
		{
			CullVolume->SetVolumeVisible(false);
		}
	}

	// Подсветку выделения гасим самим компонентом, не трогая SelectedCells:
	// выделение - это состояние работы, снимок не повод его терять.
	if (SelectionMeshComponent)
	{
		bPhotoRestoreSelectionVisible = SelectionMeshComponent->IsVisible();
		if (bPhotoRestoreSelectionVisible)
		{
			SelectionMeshComponent->SetVisibility(false);
		}
	}
}

void AAutomataOrchestrator::RestoreEditingVisualsAfterPhoto()
{
	if (ARenderCullVolume* CullVolume = EnsureRenderCullVolume())
	{
		if (bPhotoRestoreVolumeVisible)
		{
			CullVolume->SetVolumeVisible(true);
		}
		if (bPhotoRestoreGizmoVisible)
		{
			CullVolume->SetGizmoVisible(true);
		}
	}

	if (SelectionMeshComponent && bPhotoRestoreSelectionVisible)
	{
		SelectionMeshComponent->SetVisibility(true);
	}

	bPhotoRestoreVolumeVisible = false;
	bPhotoRestoreGizmoVisible = false;
	bPhotoRestoreSelectionVisible = false;
}

void AAutomataOrchestrator::ReportPhotoShotCompleted()
{
	const double ElapsedSeconds = FPlatformTime::Seconds() - PhotoShotIssuedSeconds;
	PhotoShotIssuedSeconds = 0.0;
	PhotoShotIssuedFrame = 0;

	// Ищем сам файл, а не верим, что он появился: съёмка молча не сохраняет,
	// например когда размер не влез в память, и "готово" без файла было бы
	// худшим из возможных отчётов.
	const FString ScreenshotDir = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir());
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *ScreenshotDir, TEXT("*.png"), true, false);

	FString NewestFile;
	FDateTime NewestTime = FDateTime::MinValue();
	for (const FString& File : Files)
	{
		const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*File);
		if (Timestamp > NewestTime)
		{
			NewestTime = Timestamp;
			NewestFile = File;
		}
	}

	// Файл считаем "нашим", только если он свежее момента выдачи команды с
	// небольшим запасом - иначе показали бы вчерашний снимок как результат.
	const bool bFresh = !NewestFile.IsEmpty()
		&& (FDateTime::UtcNow() - NewestTime).GetTotalSeconds() < ElapsedSeconds + 30.0;

	if (bFresh)
	{
		const double FileSizeMB = double(IFileManager::Get().FileSize(*NewestFile)) / (1024.0 * 1024.0);
		UE_LOG(LogTemp, Log, TEXT("=== Снимок готов за %.1f с: %s (%.1f МБ) ==="), ElapsedSeconds, *NewestFile, FileSizeMB);
		ShowStatusMessage(StatusKey_PhotoShot, FString::Printf(TEXT("[F10] Снимок готов за %.1f с (%.1f МБ)"), ElapsedSeconds, FileSizeMB));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("=== Снимок: %.1f с прошло, но нового файла в %s не появилось. Обычная причина - размер кадра не влез в память; попробуйте меньше PhotoShotResolution или включите bPhotoLeanMemory. ==="),
			ElapsedSeconds, *ScreenshotDir);
		ShowStatusMessage(StatusKey_PhotoShot, TEXT("[F10] Снимок НЕ сохранён - смотрите лог"));
	}

	// Безусловно, вне зависимости от того, сохранился файл или нет: экран обязан
	// вернуться в то состояние, в котором его оставил пользователь.
	RestoreEditingVisualsAfterPhoto();

	// Возвращаем тик в то состояние, которое ему полагается по остальным
	// причинам (см. SetViewSliceEnabled() - там та же строка).
	SetActorTickEnabled(bEnableViewSlice || bSimulationRunning || bFastStepActive);
}

bool AAutomataOrchestrator::ValidatePhotoShotResolution(int32& OutWidth, int32& OutHeight) const
{
	OutWidth = FMath::Max(64, PhotoShotResolution.X);
	OutHeight = FMath::Max(64, PhotoShotResolution.Y);

	// HighResShot НЕ тайлит - он заводит один рендер-таргет запрошенного
	// размера (UnrealClient.cpp: DummyViewport->SizeX = GScreenshotResolutionX).
	// Значит потолок ровно один - предел RHI на сторону 2D-текстуры, и упереться
	// в него означает не "снимок поменьше", а вылет по нехватке видеопамяти,
	// уже после того как профиль применён и сетка перерисована.
	const int32 MaxSide = static_cast<int32>(GetMax2DTextureDimension());
	if (OutWidth > MaxSide || OutHeight > MaxSide)
	{
		const FString Reason = FString::Printf(
			TEXT("снимок %dx%d не влезает: предел стороны текстуры на этой видеокарте %d"),
			OutWidth, OutHeight, MaxSide);
		UE_LOG(LogTemp, Warning, TEXT("TakePhotoShot: %s - снимок отменён"), *Reason);
		ShowStatusMessage(StatusKey_PhotoShot, FString::Printf(TEXT("[F10] %s"), *Reason));
		return false;
	}

	// Мягкий порог: в предел текстуры укладывается и то, что не укладывается в
	// видеопамять. Считаем по площади, а не по стороне - именно она и растёт
	// квадратично, и именно на ней погорела первая версия (ScreenPercentage 200
	// поверх 8K). Не запрещаем: сколько потянет конкретная карта, отсюда не
	// видно - но предупредить обязаны ДО того, как редактор начнёт умирать.
	constexpr double SoftLimitMegapixels = 40.0;
	const double Megapixels = (double(OutWidth) * double(OutHeight)) / 1000000.0;
	if (Megapixels > SoftLimitMegapixels)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePhotoShot: %.1f Мпикс (%dx%d) - это уже гигабайты видеопамяти под GBuffer. Если редактор вылетит, снижайте PhotoShotResolution."),
			Megapixels, OutWidth, OutHeight);
	}

	return true;
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
		bCullingActive ? TEXT("") : TEXT("   (отсечение НЕ активно - включить на C)")));

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

	// Через решётку, а не умножением на CellSize вручную: сдвиг на клетку
	// вдоль растянутой оси длиннее, чем в плоскости, иначе куб уезжал бы на
	// полклетки и вставал между слоями. GridDeltaToWorld(), а не
	// GridToWorld(), потому что это РАЗНОСТЬ - начало координат решётки в ней
	// обязано сократиться.
	const FVector WorldDelta = Grid ? Grid->GetLattice().GridDeltaToWorld(CellDelta) : FVector(CellDelta) * CellSize;
	const FVector NewLocation = CullVolume->GetActorLocation() + WorldDelta;
	CullVolume->SetActorLocation(NewLocation);

	// Как и в MoveCullVolumeToSelection(): программный SetActorLocation() не
	// поднимает PostEditMove(), так что перерисовываем сами. Сообщение тоже
	// оттуда - куб мог переехать, но если отсечение не активно, на экране
	// ничего не изменится, и это неотличимо от несработавшей клавиши.
	const bool bCullingActive = GetActiveCullVolume() != nullptr;
	ShowStatusMessage(StatusKey_CullVolume, FString::Printf(TEXT("Куб отсечения: %s%s"),
		*NewLocation.ToCompactString(),
		bCullingActive ? TEXT("") : TEXT("   (отсечение НЕ активно - включить на C)")));

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
		UE_LOG(LogTemp, Warning, TEXT("StartFastStep: симуляция уже запущена через Play (пробел) - остановите её сначала"));
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
		// Play по пустой сцене сначала строит состояние - тем же генератором,
		// что старт и N.
		GenerateState();
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