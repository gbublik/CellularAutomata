// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Automata/Generation/StateGenerators.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "Ui/MainHudWidget.h"
#include "Engine/Engine.h"
// RHIGetTextureMemoryStats() - видеопамять для индикатора, см. RefreshMemoryStats().
#include "DynamicRHI.h"
#include "HAL/PlatformMemory.h"

// Сглаженный FPS движка - определён в UnrealEngine.cpp, без публичного
// заголовка, объявляется локально там, где используется (тот же паттерн,
// что и в самом движке, см. напр. EngineAnalyticsSessionSummary.cpp) - см.
// FHudStats::CurrentFPS в Tick().
extern ENGINE_API float GAverageFPS;

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
	LastHudStats.GenerationsPerDispatch = LastDispatchGenerations;
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
	// Только при выбранном Gpu: на Cpu откатываться не с чего, и флаг,
	// оставшийся истинным с прошлого прогона, врал бы после переключения.
	LastHudStats.bComputeFellBackToCpu = (ComputeMethod == EComputeMethod::Gpu) && bLastComputeFellBackToCpu;
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

	// Действующее правило текстом. Через GetActiveRuleString(), т.е. из живых
	// массивов, а не из UPROPERTY RuleString - см. doc-comment поля. Собирается
	// каждый тик, а не кэшируется на применении правила: правило меняют не
	// только ApplyRuleString()/пресеты, но и правка массивов прямо в Details
	// panel, а её ловить нечем - это тот же принцип "читать заново, не
	// кэшировать", по которому живёт BuildRule(). Цена - Printf по двум
	// массивам не длиннее 27 элементов, на фоне остального в этой сводке
	// (оценка генератора, обход камеры) она не видна.
	LastHudStats.RuleString = GetActiveRuleString();

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

	// Через тот же аксессор, которым фильтр проверяет сам рендер, - "пустой
	// список значит выключен" записано ровно в одном месте.
	LastHudStats.bAgeFilterActive = IsAgeFilterActive();

	// Звук. Форма кривой берётся из ПОСЛЕДНЕГО измерения компонента, а не
	// считается тут заново: мерить одно и то же дважды за кадр незачем, а
	// главное - HUD обязан показывать ровно то, чем сейчас ведётся звук, иначе
	// глаза и уши разошлись бы и сверять их стало бы нечем.
	LastHudStats.bSonificationEnabled = bEnableSonification;
	LastHudStats.SonificationPresetName = GetActiveSonificationPresetName();
	LastHudStats.SonificationShapeName = bEnableSonification ? GetSonificationShapeName() : FString();

	// Память - раз в секунду, а не каждый тик: опрос ходит к драйверу и в
	// систему (см. RefreshMemoryStats()).
	RefreshMemoryStats();

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

	// И новая траектория: правки от прошлого прогона к ней не относятся, а
	// поколения, на которых они сделаны, начинают отсчёт заново - оставь их, и
	// первый же откат наложил бы старую правку в новую точку. Здесь же
	// снимается признак переполнения: журнал снова полон, потому что пуст.
	EditJournal.Reset();
	EditRedoStack.Reset();
	bEditJournalOverflowed = false;

	// И окно детектора застоя: замеры от прошлого сида к новому не относятся
	// вовсе, а запомненный центр габарита - тем более. Оставь их, и первый же
	// сид, случайно совпавший по численности с предыдущим, был бы объявлен
	// застойным на второй проверке (см. bAutoReseedOnStasis).
	StasisWindow.Clear();
	bHaveStasisCenter = false;
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

TArray<FAgeFilterSwatch> AAutomataOrchestrator::GetAgeFilterSwatches() const
{
	// По числу клавиш цифрового ряда, а не возрастов: возрастов 256, и хвост
	// сворачивается в последний квадратик (см. BuildAgeFilterMask()).
	constexpr int32 SwatchCount = 10;

	// Та же самая маска, по которой рисуется сетка, - не пересчитанная заново.
	TArray<bool> Mask;
	const bool bFilterActive = BuildAgeFilterMask(Mask);

	// Тот же делитель, что в BuildAgeColorLut(): цвет квадратика обязан
	// совпадать с цветом клетки на экране, иначе легенда легендой не является.
	const float MaxAge = float(FMath::Max(1, AgeColorMaxAge));

	TArray<FAgeFilterSwatch> Swatches;
	Swatches.Reserve(SwatchCount);

	for (int32 Age = 0; Age < SwatchCount; ++Age)
	{
		FAgeFilterSwatch Swatch;
		Swatch.Age = Age;
		Swatch.bIncludesOlder = (Age == SwatchCount - 1);
		Swatch.Color = SampleColorRamp(AgeColors, float(Age) / MaxAge);

		// У хвоста дальний конец - конец рампы: он покрывает 9..255, то есть
		// почти всю её, и одним цветом был бы враньём. У остальных совпадает с
		// Color, чтобы вёрстке не приходилось разбирать частные случаи.
		Swatch.ColorEnd = Swatch.bIncludesOlder
			? SampleColorRamp(AgeColors, 1.0f)
			: Swatch.Color;

		// Фильтр снят - видно всё. Иначе спрашиваем маску, а не список
		// выбранных: правило "и всё, что старше" в списке не записано.
		Swatch.bVisible = !bFilterActive || Mask[Age];

		if (GamePC)
		{
			const FKey Key = GamePC->KeyFor((EHotkey)((int32)EHotkey::AgeFilter0 + Age));
			Swatch.KeyLabel = Key.GetDisplayName().ToString();
		}

		Swatches.Add(MoveTemp(Swatch));
	}

	return Swatches;
}

void AAutomataOrchestrator::RefreshMemoryStats()
{
	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds - LastMemoryStatsSeconds < MemoryStatsIntervalSeconds)
	{
		return;
	}
	LastMemoryStatsSeconds = NowSeconds;

	constexpr double BytesPerMB = 1024.0 * 1024.0;

	// Видеопамять. Числа драйверные и приблизительные - считают текстуры и
	// рендер-таргеты, но не буферы GPU-стратегии, - и это ровно та точность,
	// которая тут нужна: индикатор, а не расходомер.
	FTextureMemoryStats TextureStats;
	RHIGetTextureMemoryStats(TextureStats);

	LastHudStats.VideoMemoryUsedMB =
		double(TextureStats.StreamingMemorySize + TextureStats.NonStreamingMemorySize) / BytesPerMB;

	// -1 означает "драйвер не сказал", и превращать это в отрицательные
	// мегабайты нельзя: ноль читается виджетом как "неизвестно".
	LastHudStats.VideoMemoryTotalMB = TextureStats.TotalGraphicsMemory > 0
		? double(TextureStats.TotalGraphicsMemory) / BytesPerMB
		: 0.0;

	LastHudStats.VideoMemoryLargestFreeBlockMB = TextureStats.LargestContiguousAllocation > 0
		? double(TextureStats.LargestContiguousAllocation) / BytesPerMB
		: 0.0;

	// Оперативная память - по системе целиком, а не по процессу: массив
	// кандидатов CPU-пути упирается именно в физическую память машины.
	const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
	LastHudStats.SystemMemoryAvailableMB = double(MemoryStats.AvailablePhysical) / BytesPerMB;
	LastHudStats.SystemMemoryTotalMB = double(MemoryStats.TotalPhysical) / BytesPerMB;
}
