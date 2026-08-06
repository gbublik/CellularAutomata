#pragma once

#include "CoreMinimal.h"
#include "Automata/Rendering/ChunkedRenderOrder.h"
#include "Automata/Simulation/ComputeMethod.h"
#include "HudStats.generated.h"

/** Сводка для HUD (см. AAutomataOrchestrator::GetHudStats()) - USTRUCT(BlueprintType)
 *  с BlueprintReadOnly-полями, читает её UMG/Blueprint-виджет (UMainHudWidget).
 *  Считается один раз за тик/шаг и хранится на оркестраторе - виджет её
 *  просто читает через GetHudStats(), ничего сам не пересчитывает. */
USTRUCT(BlueprintType)
struct FHudStats
{
	GENERATED_BODY()

	/** Фоновый StepAsync()/Next() сейчас считает поколение - см. bStepInProgress. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bIsComputing = false;

	/** Чанковый рендер сейчас "разливается" по кадрам - см. bChunkedRenderInProgress. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bIsRendering = false;

	/** Сглаженный FPS движка (GAverageFPS) - не считаем сами, берём готовое. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	float CurrentFPS = 0.0f;

	/** Сколько поколений посчитано с последнего GenerateRandom()/
	 *  ResetToInitialState() - см. AAutomataOrchestrator::GenerationCount. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int64 GenerationCount = 0;

	/** Скользящая частота поколений в секунду - обновляется раз в секунду,
	 *  не каждый кадр (см. UpdateGenerationsPerSecond()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	float GenerationsPerSecond = 0.0f;

	/** Простая оценка объёма данных, загружаемых в GPU-буфер для
	 *  compute-шейдера на последнем шаге (FGpuComputeStrategy::Step()'s
	 *  битовый входной буфер) - 0, если последний шаг считался на CPU
	 *  (там нет такой загрузки вовсе) или сетка ещё не запускалась.
	 *  Специально НЕ пересчитывается отдельным сканированием сетки -
	 *  FGpuComputeStrategy и так строит этот буфер каждый GPU-шаг, здесь
	 *  просто читается уже посчитанное им число (см.
	 *  FCellularAutomatonComputeStrategy::GetLastComputeUploadBytes()),
	 *  без лишней нагрузки на многомиллионных сетках. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	double EstimatedGpuComputeUploadMB = 0.0;

	/** Сколько клеток сейчас живо во всей сетке - Grid->Num(), О(1) (счётчик
	 *  ведут сами чанки, полного скана нет). Отличается от
	 *  FCellRenderStats::TotalCellCount тем, что обновляется каждый тик, а не
	 *  только на рендере: после Delete/выделения число меняется сразу, ещё до
	 *  следующего поколения. 0, если сетка не создана (в т.ч. после
	 *  BakeCellsToMesh(), который её освобождает). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 AliveCellCount = 0;

	/** Сколько клеток сейчас в выделении (см. SelectedCells) - HUD'у, чтобы
	 *  показывать, есть ли что извлекать/удалять, и сколько именно. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 SelectedCellCount = 0;

	/** Заданная скорость симуляции - AAutomataOrchestrator::Speed, т.е.
	 *  ЦЕЛЕВОЕ число поколений в секунду (крутится +/-). Отличается от
	 *  GenerationsPerSecond выше, которое измеряет ФАКТИЧЕСКУЮ частоту: на
	 *  больших сетках одно поколение считается дольше 1/Speed, и фактическая
	 *  оказывается кратно ниже заданной. Оба поля нужны именно в паре - по
	 *  расхождению между ними и видно, что симуляция упёрлась в вычисления, а
	 *  не в настройку. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	float SimulationSpeed = 0.0f;

	/** Сколько поколений считается на один рендер - AAutomataOrchestrator::
	 *  StepsPerRender (крутится T/G). 1 - рисуется каждое поколение. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 StepsPerRender = 1;

	/** Текущая максимальная скорость полёта камеры, юнитов в секунду -
	 *  читается прямо с UFloatingPawnMovement::MaxSpeed управляемого пешки, а
	 *  не считается из CameraSpeedMultiplier: пока держат Left Shift,
	 *  контроллер уже умножил MaxSpeed (см. AGamePlayerController::
	 *  OnSpeedBoostStarted()), и HUD должен показывать фактическую скорость
	 *  вместе с ускорением, а не базовую. 0, если пешка/её движение ещё не
	 *  готовы (в т.ч. вне PIE). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	float CameraSpeed = 0.0f;

	/** Ширина контура на грани клетки - зеркало AAutomataOrchestrator::
	 *  CellBorderWidth, чтобы слайдер в HUD показывал текущее значение, а не
	 *  держал свою копию (тот же принцип, что у SimulationSpeed/StepsPerRender).
	 *  Меняется через SetCellBorderWidth(). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	float CellBorderWidth = 0.0f;

	// --- Режимы работы: зеркала живых переключателей, по одному на хоткей ---
	// Все они зеркала, а не отдельное состояние: HUD показывает ровно то, что
	// переключают хоткеи и Details panel. Держатся здесь, в FHudStats, а не в
	// отдельной структуре, ровно потому, что виджету так нужен один Break-нод
	// вместо двух, и потому же, что сюда уже переехали заданные Speed/
	// StepsPerRender - "настройка" и "измерение" в этой сводке живут рядом
	// осознанно (см. SimulationSpeed).

	/** Идёт непрерывный прогон (пробел) - см. bSimulationRunning. Взаимоисключающ с
	 *  bFastStepActive ниже. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bSimulationRunning = false;

	/** Держат Shift+F - автошаг, см. bFastStepActive/StartFastStep(). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bFastStepActive = false;

	/** Режим взаимодействия мышью (Tab): камера стоит, курсор виден, работают
	 *  рамка выделения и клики по HUD. Читается с контроллера
	 *  (AGamePlayerController::IsSelectionModeActive()), а не хранится тут -
	 *  false, если контроллер ещё не готов. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bSelectionModeActive = false;

	/** ВЫБРАННЫЙ метод расчёта - см. ComputeMethod. Именно выбранный, а не тот,
	 *  которым посчитан последний шаг: см. bComputeFellBackToCpu ниже. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	EComputeMethod ComputeMethod = EComputeMethod::Cpu;

	/** Выбран Gpu, но последний шаг посчитан на CPU: стратегия откатилась, не
	 *  влезши в свои лимиты (см.
	 *  FCellularAutomatonComputeStrategy::DidLastStepFallBackToCpu() - соседей
	 *  больше, чем держит шейдер; объём AABB не влез в GpuVolumeCellLimit;
	 *  объём вышел за 32-битную индексацию).
	 *
	 *  Та же разница, что между bViewSliceEnabled и bViewSliceActive, и тот же
	 *  повод: ComputeMethod показывает НАМЕРЕНИЕ и продолжал говорить "Gpu",
	 *  пока считал CPU, - откат был виден только предупреждением в логе. А
	 *  замечать его стоит: он объясняет, почему шаг вдруг стал в разы дольше.
	 *
	 *  Виджету это скорее пометка рядом с ComputeMethod, чем отдельная строка -
	 *  та же идиома, что звёздочка bRenderPresetModified. Описывает ПОСЛЕДНИЙ
	 *  шаг: объём растёт вместе с сеткой, так что флаг может зажечься посреди
	 *  прогона и больше не погаснуть. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bComputeFellBackToCpu = false;

	/** Разлитый по кадрам рендер (Z) - см. bEnableChunkedRender. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bChunkedRenderEnabled = false;

	/** Порядок появления клеток при разливе (X) - см. EChunkedRenderOrder. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	EChunkedRenderOrder ChunkedRenderOrder = EChunkedRenderOrder::Sequential;

	/** Ждать дорисовки разлива перед следующим шагом (V) - см.
	 *  bWaitForChunkedRenderToFinish. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bWaitForChunkedRenderToFinish = false;

	/** Отсечение клеток по расстоянию (B) - см. bEnableCellCulling. Само по
	 *  себе НЕ означает, что клетки сейчас режутся: см. bCellCullingActive. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bCellCullingEnabled = false;

	/** Итог: отсечение по расстоянию СЕЙЧАС реально работает - переключатель
	 *  включён И CellCullEndDistance > 0.
	 *
	 *  Отдельное поле по той же причине, что и bCullVolumeActive ниже, только
	 *  случай тут ещё неприятнее: bEnableCellCulling по умолчанию true, а обе
	 *  дистанции - 0 (движковый "выключено"). То есть индикатор, повешенный на
	 *  один переключатель, горел бы с первой секунды сессии, ничего при этом не
	 *  отсекая - ровно то враньё, ради недопущения которого этот блок
	 *  зеркалит ФАКТИЧЕСКИЕ состояния, а не только тумблеры. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bCellCullingActive = false;

	/** Отсечение кубом включено (C) - см. bEnableRenderCullVolume. Само по себе
	 *  НЕ означает, что клетки сейчас режутся: см. bCullVolumeActive ниже. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bRenderCullVolumeEnabled = false;

	/** Куб виден (Ctrl+C) - ARenderCullVolume::IsVolumeVisible(). false и в
	 *  случае, когда ARenderCullVolume на уровне просто нет. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bRenderCullVolumeVisible = false;

	/** Итог: клетки СЕЙЧАС реально режутся кубом - конъюнкция "актёр есть на
	 *  уровне И включено (C)", т.е. ровно то, что отвечает
	 *  GetActiveCullVolume(). Отдельное поле, а не зеркало
	 *  bRenderCullVolumeEnabled: наличие актёра на уровне из переключателя не
	 *  выводится, а именно оно и делает разницу между "включено" и "работает".
	 *  Видимость куба (bRenderCullVolumeVisible выше) сюда НЕ входит -
	 *  спрятанный куб режет так же, как и видимый. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bCullVolumeActive = false;

	/** Срез вдоль взгляда включён (J) - см. bEnableViewSlice. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bViewSliceEnabled = false;

	/** Итог: срез СЕЙЧАС реально режет - включён И камера доступна
	 *  (GetCameraView()). Второе условие не формальность: плоскость среза
	 *  задаётся положением и направлением камеры, и без них резать нечем -
	 *  BuildCellRenderData() проверяет ровно эту же конъюнкцию.
	 *
	 *  Троица bCellCullingActive/bCullVolumeActive/bViewSliceActive - это три
	 *  индикатора HUD, и все три намеренно про "режет", а не про "включено":
	 *  тумблеры рядом (bCellCullingEnabled/bRenderCullVolumeEnabled/
	 *  bViewSliceEnabled) остаются на месте, если виджету захочется
	 *  промежуточного состояния "взведён, но не работает". */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bViewSliceActive = false;

	/** Фильтр по возрасту (цифровой ряд) сейчас что-то прячет - четвёртый в той
	 *  же троице индикаторов "режет", ставший четвёркой.
	 *
	 *  Пары "включён/режет" у него нет, в отличие от соседей: фильтр задаётся
	 *  списком возрастов, и пустой список - это и есть "выключен", отдельному
	 *  тумблеру взводиться нечем.
	 *
	 *  Нужен затем, что AliveCellCount считает ЖИВЫХ, а на экране при фильтре
	 *  видна только их часть - без этого флага число и картинка расходятся, и
	 *  объяснить расхождение нечем: всплывающее сообщение о фильтре гаснет
	 *  через несколько секунд после нажатия. Какие именно слои показаны, отдаёт
	 *  AAutomataOrchestrator::GetAgeFilterSwatches(); здесь только "фильтр
	 *  вообще есть", чтобы виджету хватило одного bool для индикатора. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bAgeFilterActive = false;

	/** Chunk-силуэт (H) - см. bEnableGhostShape. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bGhostShapeEnabled = false;

	/** Силуэт сейчас ЗАМЕНЯЕТ поклеточный рендер, а не дополняет его - см.
	 *  ShouldGhostShapeReplaceDetailedRender(). Именно этот режим и даёт
	 *  выигрыш в скорости, поэтому его видно отдельно от bGhostShapeEnabled. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bGhostShapeReplacesDetailedRender = false;

	/** Имя последнего применённого профиля рендера (F1-F4) - см.
	 *  FRenderPreset/ApplyRenderPreset(). Пустая строка, пока ни один не
	 *  применяли. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	FString RenderPresetName;

	/** Его индекс в RenderPresets::GetAll(), либо INDEX_NONE. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 RenderPresetIndex = INDEX_NONE;

	/** После применения профиля что-то из его настроек поменяли вручную
	 *  (хоткеями B/C/H/U или в Details panel) - т.е. на экране УЖЕ не то, что
	 *  описывает RenderPresetName. Отдельное поле, потому что иначе HUD
	 *  показывал бы "Performance" на картинке, которая ей больше не
	 *  соответствует; виджету достаточно дорисовать звёздочку. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bRenderPresetModified = false;

	/** Действующее правило строкой "Survival/Birth/States/Neighborhood" -
	 *  например "4-5/5/2/M" (см. RuleStringParser.h за точной семантикой).
	 *
	 *  Берётся из GetActiveRuleString(), то есть собирается из живых
	 *  SurvivalCounts/BirthCounts/States/Neighborhood, а НЕ из UPROPERTY
	 *  RuleString: то поле - строка ввода, оно остаётся тем, что набрали
	 *  последний раз, и после правки массивов руками или применения пресета
	 *  описывает уже не тот автомат, который считается. HUD, показывающий
	 *  устаревшее правило, хуже HUD, не показывающего правила вовсе.
	 *
	 *  Здесь, в общей сводке, а не отдельным вызовом GetActiveRuleString() из
	 *  виджета - по той же причине, что и остальные зеркала: один Break-нод на
	 *  весь HUD (см. блок режимов выше). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	FString RuleString;

	/** Выбранный генератор начального состояния - что построит Y (см.
	 *  StateGenerators::GetDisplayName()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	FString StateGeneratorName;

	/** Сколько клеток он даст - оценка без построения, чтобы было видно ДО
	 *  нажатия, во что обойдётся нажатие. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int64 EstimatedGeneratorCells = 0;

	/** Клетки отбрасывают тени - см. bCellsCastShadows. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bCellsCastShadows = true;

	/** Фон (небо/туман) виден (U) - см. bShowBackground. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bBackgroundVisible = true;

	/** Ортогональная проекция (NumPad 5). Читается с контроллера
	 *  (AGamePlayerController::IsOrthographicCamera()), как и
	 *  bSelectionModeActive выше: проекция - состояние камеры, а не автомата. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bOrthographicCamera = false;

	/** Лампочка на камере (Shift+U). Тоже читается с контроллера - свет висит
	 *  на пешке, а не на автомате (AGamePlayerController::IsHeadlightEnabled()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bHeadlightEnabled = false;

	/** Автоперебор сидов по вымиранию (Shift+N) и сколько сидов он уже
	 *  перебрал - см. AAutomataOrchestrator::bAutoReseedOnExtinction. Счётчик
	 *  здесь потому, что режим включают и уходят: единственное, что о нём
	 *  хочется знать издалека, - идёт ли перебор и на какой он попытке. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bAutoReseedOnExtinction = false;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 AutoReseedCount = 0;

	/** Озвучивание симуляции (P) и применённый набор настроек (Shift+P). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bSonificationEnabled = false;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	FString SonificationPresetName;

	/** Форма кривой населения словом - "Взрывной рост", "Насыщение", "Обвал".
	 *
	 *  Это то же измерение, которым ведётся звук, только выведенное текстом:
	 *  услышать разницу между разгоном и насыщением можно сразу, а вот
	 *  убедиться, что подсистема поняла её так же, как ухо, - только глазами.
	 *  Пусто, когда звук выключен. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	FString SonificationShapeName;
};
