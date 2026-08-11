#pragma once

#include "CoreMinimal.h"
#include "Automata/Rendering/ChunkedRenderOrder.h"
#include "Automata/Simulation/ComputeMethod.h"
#include "HudStats.generated.h"

/** Сводки для HUD - по одной на подсистему, каждая со своим BlueprintPure-
 *  геттером на оркестраторе (GetSimulationStats(), GetHudRenderStats(),
 *  GetCutStats(), GetPerformanceStats(), GetCameraStats(),
 *  GetGeneratorStats(), GetSonificationStats()).
 *
 *  Все семь - USTRUCT(BlueprintType) с BlueprintReadOnly-полями: считаются один
 *  раз за кадр в UpdateHudStats() и хранятся на оркестраторе, виджет их только
 *  читает и ничего не пересчитывает сам.
 *
 *  Раньше это была ОДНА структура FHudStats на полсотни полей (удалена, когда
 *  последний провод в WBP_MainHud переложили на эти семь), и держалась она одной
 *  ровно ради одного Break-нода в графе виджета. Довод не выдержал роста: к
 *  полусотне полей Break-нод перестаёт помещаться на экран, и всякая панель
 *  HUD тянет провод через весь граф мимо сорока чужих пинов. Панелей же в HUD
 *  как раз столько, сколько здесь структур, и каждая читает свою.
 *
 *  Цена размена записана честно: раскладка по подсистемам - это раскладка ПО
 *  ТЕМЕ, а не по природе величины. Внутри каждой структуры соседствуют
 *  "измерение" (сколько получилось) и "настройка" (сколько попросили) -
 *  GenerationsPerSecond рядом с SimulationSpeed, GenerationsPerDispatch рядом
 *  со StepsPerRender. Это сделано намеренно: смысл у таких полей появляется
 *  только в паре, разведи их по разным нодам - и расхождение, ради которого
 *  они оба заведены, станет некому показать. */

/** Симуляция: идёт ли прогон, сколько поколений, с какой частотой и по какому
 *  правилу. Всё, что описывает автомат, а не то, как его рисуют. */
USTRUCT(BlueprintType)
struct FHudSimulationStats
{
	GENERATED_BODY()

	/** Идёт непрерывный прогон (пробел) - см. bSimulationRunning. Взаимоисключающ с
	 *  bFastStepActive ниже. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	bool bSimulationRunning = false;

	/** Держат Shift+F - автошаг, см. bFastStepActive/StartFastStep(). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	bool bFastStepActive = false;

	/** Фоновый StepAsync()/Next() сейчас считает поколение - см. bStepInProgress.
	 *  Пара к FHudRenderStats::bIsRendering: это разные фазы, они могут идти
	 *  одновременно, и разъехались по разным структурам именно поэтому - "считаем"
	 *  относится к симуляции, "рисуем" к рендеру. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	bool bIsComputing = false;

	/** Сколько поколений посчитано с последнего GenerateRandom()/
	 *  ResetToInitialState() - см. AAutomataOrchestrator::GenerationCount. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	int64 GenerationCount = 0;

	/** Скользящая частота поколений в секунду - обновляется раз в секунду,
	 *  не каждый кадр (см. UpdateGenerationsPerSecond()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	float GenerationsPerSecond = 0.0f;

	/** Заданная скорость симуляции - AAutomataOrchestrator::Speed, т.е.
	 *  ЦЕЛЕВОЕ число поколений в секунду (крутится +/-). Отличается от
	 *  GenerationsPerSecond выше, которое измеряет ФАКТИЧЕСКУЮ частоту: на
	 *  больших сетках одно поколение считается дольше 1/Speed, и фактическая
	 *  оказывается кратно ниже заданной. Оба поля нужны именно в паре - по
	 *  расхождению между ними и видно, что симуляция упёрлась в вычисления, а
	 *  не в настройку. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	float SimulationSpeed = 0.0f;

	/** Сколько поколений ЗАПРОШЕНО на один рендер - AAutomataOrchestrator::
	 *  StepsPerRender (крутится T/G). 1 - рисуется каждое поколение.
	 *
	 *  Именно запрошено: сколько сложилось на деле, см. GenerationsPerDispatch.
	 *
	 *  Живёт в симуляции, а не в рендере, хотя названием тянет туда: крутит оно
	 *  темп счёта, а рендер только пропускает кадры вслед за ним. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	int32 StepsPerRender = 1;

	/** Сколько поколений реально продвинул последний фоновый заход.
	 *
	 *  Пара к StepsPerRender выше, и пара осмысленная: GPU-стратегия считает
	 *  пачку за один круг, только пока объём AABB влезает в её потолок, а объём
	 *  растёт вместе со структурой. Как только перестаёт - пачка урезается до
	 *  одного поколения, и в логе появляется "поколений за круг: 1 из 3". До
	 *  этого поля лог был единственным местом, где это было видно: HUD показывал
	 *  запрошенные 3 и молчал о том, что считается по одному.
	 *
	 *  Заметить стоит, потому что это не мелочь производительности: заход в этом
	 *  режиме идёт секунды вместо миллисекунд, а Tick() ещё и пересчитывает по
	 *  этому числу темп, чтобы Speed продолжал означать поколения в секунду, а
	 *  не заходы. Это же и ранний признак, что скоро начнутся откаты на CPU -
	 *  см. FHudPerformanceStats::bComputeFellBackToCpu.
	 *
	 *  Ручной шаг (F) догоняет запрошенное число полностью, даже если для этого
	 *  придётся сделать несколько кругов, - там равенство со StepsPerRender
	 *  норма. Расхождение это признак непрерывного прогона. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	int32 GenerationsPerDispatch = 1;

	/** Сколько клеток сейчас живо во всей сетке - Grid->Num(), О(1) (счётчик
	 *  ведут сами чанки, полного скана нет). Отличается от
	 *  FCellRenderStats::TotalCellCount тем, что обновляется каждый тик, а не
	 *  только на рендере: после Delete/выделения число меняется сразу, ещё до
	 *  следующего поколения. 0, если сетка не создана (в т.ч. после
	 *  BakeCellsToMesh(), который её освобождает). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	int32 AliveCellCount = 0;

	/** Действующее правило строкой "Survival/Birth/States/Neighborhood" -
	 *  например "4-5/5/2/M" (см. RuleStringParser.h за точной семантикой).
	 *
	 *  Берётся из GetActiveRuleString(), то есть собирается из живых
	 *  SurvivalCounts/BirthCounts/States/Neighborhood, а НЕ из UPROPERTY
	 *  RuleString: то поле - строка ввода, оно остаётся тем, что набрали
	 *  последний раз, и после правки массивов руками или применения пресета
	 *  описывает уже не тот автомат, который считается. HUD, показывающий
	 *  устаревшее правило, хуже HUD, не показывающего правила вовсе. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	FString RuleString;

	/** Автоперебор сидов по вымиранию (Shift+N) и сколько сидов он уже
	 *  перебрал - см. AAutomataOrchestrator::bAutoReseedOnExtinction. Счётчик
	 *  здесь потому, что режим включают и уходят: единственное, что о нём
	 *  хочется знать издалека, - идёт ли перебор и на какой он попытке. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	bool bAutoReseedOnExtinction = false;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Simulation")
	int32 AutoReseedCount = 0;
};

/** Рендер: как именно клетки попадают на экран - разлив по кадрам, профиль,
 *  тени, фон, силуэт. Что при этом ВЫРЕЗАЕТСЯ, живёт отдельно, в FHudCutStats:
 *  отсечения - это не настройка картинки, а три независимых механизма, у
 *  каждого своя пара "включено/режет". */
USTRUCT(BlueprintType)
struct FHudRenderStats
{
	GENERATED_BODY()

	/** Чанковый рендер сейчас "разливается" по кадрам - см.
	 *  bChunkedRenderInProgress. Пара к FHudSimulationStats::bIsComputing. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	bool bIsRendering = false;

	/** Разлитый по кадрам рендер (Z) - см. bEnableChunkedRender. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	bool bChunkedRenderEnabled = false;

	/** Порядок появления клеток при разливе (X) - см. EChunkedRenderOrder. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	EChunkedRenderOrder ChunkedRenderOrder = EChunkedRenderOrder::Sequential;

	/** Ждать дорисовки разлива перед следующим шагом (V) - см.
	 *  bWaitForChunkedRenderToFinish. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	bool bWaitForChunkedRenderToFinish = false;

	/** Имя последнего применённого профиля рендера (F1-F4) - см.
	 *  FRenderPreset/ApplyRenderPreset(). Пустая строка, пока ни один не
	 *  применяли. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	FString RenderPresetName;

	/** Его индекс в RenderPresets::GetAll(), либо INDEX_NONE. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	int32 RenderPresetIndex = INDEX_NONE;

	/** После применения профиля что-то из его настроек поменяли вручную
	 *  (хоткеями B/C/H/U или в Details panel) - т.е. на экране УЖЕ не то, что
	 *  описывает RenderPresetName. Отдельное поле, потому что иначе HUD
	 *  показывал бы "Performance" на картинке, которая ей больше не
	 *  соответствует; виджету достаточно дорисовать звёздочку. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	bool bRenderPresetModified = false;

	/** Клетки отбрасывают тени - см. bCellsCastShadows. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	bool bCellsCastShadows = true;

	/** Фон (небо/туман) виден (U) - см. bShowBackground. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	bool bBackgroundVisible = true;

	/** Ширина контура на грани клетки - зеркало AAutomataOrchestrator::
	 *  CellBorderWidth, чтобы слайдер в HUD показывал текущее значение, а не
	 *  держал свою копию (тот же принцип, что у SimulationSpeed/StepsPerRender).
	 *  Меняется через SetCellBorderWidth(). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	float CellBorderWidth = 0.0f;

	/** Chunk-силуэт (H) - см. bEnableGhostShape. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	bool bGhostShapeEnabled = false;

	/** Силуэт сейчас ЗАМЕНЯЕТ поклеточный рендер, а не дополняет его - см.
	 *  ShouldGhostShapeReplaceDetailedRender(). Именно этот режим и даёт
	 *  выигрыш в скорости, поэтому его видно отдельно от bGhostShapeEnabled. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Render")
	bool bGhostShapeReplacesDetailedRender = false;
};

/** Отсечения - четыре механизма, убирающие клетки с экрана, и четыре
 *  индикатора HUD: Culling (B, по расстоянию), Volume (C, куб), Slab (J, срез
 *  вдоль взгляда) и фильтр по возрасту (цифровой ряд).
 *
 *  Терминология тут различает вещи, которые легко спутать (см. docs/rendering.md):
 *  отсечение по расстоянию прячет инстансы ПОСЛЕ того, как они построены, а куб
 *  и срез выбрасывают клетки внутри BuildCellRenderData(), до того как строить
 *  хоть что-то.
 *
 *  У трёх из четырёх поля идут парой "включено"/"режет", и все индикаторы
 *  повешены на второе, а не на первое: bEnableCellCulling по умолчанию true при
 *  нулевых дистанциях, куба может не быть на уровне вовсе, а срезу нужна
 *  камера. Тумблеры оставлены рядом на случай, если вёрстке захочется третьего,
 *  притушенного состояния "взведён, но не работает". */
USTRUCT(BlueprintType)
struct FHudCutStats
{
	GENERATED_BODY()

	/** Отсечение клеток по расстоянию (B) - см. bEnableCellCulling. Само по
	 *  себе НЕ означает, что клетки сейчас режутся: см. bCellCullingActive. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Cuts")
	bool bCellCullingEnabled = false;

	/** Итог: отсечение по расстоянию СЕЙЧАС реально работает - переключатель
	 *  включён И CellCullEndDistance > 0.
	 *
	 *  Отдельное поле по той же причине, что и bCullVolumeActive ниже, только
	 *  случай тут ещё неприятнее: bEnableCellCulling по умолчанию true, а обе
	 *  дистанции - 0 (движковый "выключено"). То есть индикатор, повешенный на
	 *  один переключатель, горел бы с первой секунды сессии, ничего при этом не
	 *  отсекая - ровно то враньё, ради недопущения которого эта структура
	 *  зеркалит ФАКТИЧЕСКИЕ состояния, а не только тумблеры. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Cuts")
	bool bCellCullingActive = false;

	/** Отсечение кубом включено (C) - см. bEnableRenderCullVolume. Само по себе
	 *  НЕ означает, что клетки сейчас режутся: см. bCullVolumeActive ниже. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Cuts")
	bool bRenderCullVolumeEnabled = false;

	/** Куб виден (Ctrl+Shift+C) - ARenderCullVolume::IsVolumeVisible(). false и в
	 *  случае, когда ARenderCullVolume на уровне просто нет. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Cuts")
	bool bRenderCullVolumeVisible = false;

	/** Итог: клетки СЕЙЧАС реально режутся кубом - конъюнкция "актёр есть на
	 *  уровне И включено (C)", т.е. ровно то, что отвечает
	 *  GetActiveCullVolume(). Отдельное поле, а не зеркало
	 *  bRenderCullVolumeEnabled: наличие актёра на уровне из переключателя не
	 *  выводится, а именно оно и делает разницу между "включено" и "работает".
	 *  Видимость куба (bRenderCullVolumeVisible выше) сюда НЕ входит -
	 *  спрятанный куб режет так же, как и видимый. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Cuts")
	bool bCullVolumeActive = false;

	/** Срез вдоль взгляда включён (J) - см. bEnableViewSlice. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Cuts")
	bool bViewSliceEnabled = false;

	/** Итог: срез СЕЙЧАС реально режет - включён И камера доступна
	 *  (GetCameraView()). Второе условие не формальность: плоскость среза
	 *  задаётся положением и направлением камеры, и без них резать нечем -
	 *  BuildCellRenderData() проверяет ровно эту же конъюнкцию. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Cuts")
	bool bViewSliceActive = false;

	/** Фильтр по возрасту (цифровой ряд) сейчас что-то прячет - четвёртый в той
	 *  же четвёрке индикаторов "режет".
	 *
	 *  Пары "включён/режет" у него нет, в отличие от соседей: фильтр задаётся
	 *  списком возрастов, и пустой список - это и есть "выключен", отдельному
	 *  тумблеру взводиться нечем.
	 *
	 *  Нужен затем, что FHudSimulationStats::AliveCellCount считает ЖИВЫХ, а на
	 *  экране при фильтре видна только их часть - без этого флага число и
	 *  картинка расходятся, и объяснить расхождение нечем: всплывающее сообщение
	 *  о фильтре гаснет через несколько секунд после нажатия. Какие именно слои
	 *  показаны, отдаёт AAutomataOrchestrator::GetAgeFilterSwatches(); здесь
	 *  только "фильтр вообще есть", чтобы виджету хватило одного bool для
	 *  индикатора. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Cuts")
	bool bAgeFilterActive = false;
};

/** Производительность и ресурсы: кадры, чем считается шаг и сколько осталось
 *  памяти. Всё, на что смотрят, когда становится медленно. */
USTRUCT(BlueprintType)
struct FHudPerformanceStats
{
	GENERATED_BODY()

	/** Сглаженный FPS движка (GAverageFPS) - не считаем сами, берём готовое. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Performance")
	float CurrentFPS = 0.0f;

	/** ВЫБРАННЫЙ метод расчёта - см. ComputeMethod. Именно выбранный, а не тот,
	 *  которым посчитан последний шаг: см. bComputeFellBackToCpu ниже. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Performance")
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
	 *  та же идиома, что звёздочка FHudRenderStats::bRenderPresetModified.
	 *  Описывает ПОСЛЕДНИЙ шаг: объём растёт вместе с сеткой, так что флаг может
	 *  зажечься посреди прогона и больше не погаснуть. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Performance")
	bool bComputeFellBackToCpu = false;

	/** Простая оценка объёма данных, загружаемых в GPU-буфер для
	 *  compute-шейдера на последнем шаге (FGpuComputeStrategy::Step()'s
	 *  битовый входной буфер) - 0, если последний шаг считался на CPU
	 *  (там нет такой загрузки вовсе) или сетка ещё не запускалась.
	 *  Специально НЕ пересчитывается отдельным сканированием сетки -
	 *  FGpuComputeStrategy и так строит этот буфер каждый GPU-шаг, здесь
	 *  просто читается уже посчитанное им число (см.
	 *  FCellularAutomatonComputeStrategy::GetLastComputeUploadBytes()),
	 *  без лишней нагрузки на многомиллионных сетках. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Performance")
	double EstimatedGpuComputeUploadMB = 0.0;

	/** Видеопамять: занято сейчас и всего, МБ. Ноль в Total означает "драйвер не
	 *  сказал" (см. FTextureMemoryStats::AreHardwareStatsValid()), а не "нет
	 *  памяти" - виджету стоит в этом случае показать прочерк, а не 0%.
	 *
	 *  Считает текстуры и рендер-таргеты, то есть НЕ включает буферы, которые
	 *  GPU-стратегия заводит под шаг. Как точный расходомер не годится и не
	 *  предназначен - это индикатор "далеко ли до края", чтобы не ходить за ним
	 *  в диспетчер задач. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Performance")
	double VideoMemoryUsedMB = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Performance")
	double VideoMemoryTotalMB = 0.0;

	/** Наибольший НЕПРЕРЫВНЫЙ свободный кусок видеопамяти, МБ.
	 *
	 *  Отдельно от "сколько всего свободно", потому что вопрос у нас именно
	 *  такой: пачке нужен один буфер целиком (плоскость возрастов - байт на
	 *  клетку объёма), и суммарно свободные два гигабайта двумя кусками ей не
	 *  помогут. Это то число, по которому видно, влезет ли следующая пачка. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Performance")
	double VideoMemoryLargestFreeBlockMB = 0.0;

	/** Оперативная память: свободно и всего, МБ. Свободно - по системе целиком,
	 *  а не по процессу: CPU-путь заводит массив кандидатов на живые*(соседей+1)
	 *  элементов, и упереться там можно в физическую память машины. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Performance")
	double SystemMemoryAvailableMB = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Performance")
	double SystemMemoryTotalMB = 0.0;
};

/** Камера и взаимодействие мышью: чем сейчас управляют и что выделено.
 *
 *  Три из пяти полей читаются с AGamePlayerController, а не с оркестратора, -
 *  это состояние камеры и ввода, а не автомата, и разложены они сюда именно
 *  поэтому. Выделение стоит рядом с ними, потому что делается тоже мышью и в
 *  том же режиме (Tab). */
USTRUCT(BlueprintType)
struct FHudCameraStats
{
	GENERATED_BODY()

	/** Текущая максимальная скорость полёта камеры, юнитов в секунду -
	 *  читается прямо с UFloatingPawnMovement::MaxSpeed управляемого пешки, а
	 *  не считается из CameraSpeedMultiplier: пока держат Left Shift,
	 *  контроллер уже умножил MaxSpeed (см. AGamePlayerController::
	 *  OnSpeedBoostStarted()), и HUD должен показывать фактическую скорость
	 *  вместе с ускорением, а не базовую. 0, если пешка/её движение ещё не
	 *  готовы (в т.ч. вне PIE). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Camera")
	float CameraSpeed = 0.0f;

	/** Ортогональная проекция (NumPad 5). Читается с контроллера
	 *  (AGamePlayerController::IsOrthographicCamera()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Camera")
	bool bOrthographicCamera = false;

	/** Лампочка на камере (Shift+U). Тоже читается с контроллера - свет висит
	 *  на пешке, а не на автомате (AGamePlayerController::IsHeadlightEnabled()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Camera")
	bool bHeadlightEnabled = false;

	/** Режим взаимодействия мышью (Tab): камера стоит, курсор виден, работают
	 *  рамка выделения и клики по HUD. Читается с контроллера
	 *  (AGamePlayerController::IsSelectionModeActive()), а не хранится тут -
	 *  false, если контроллер ещё не готов. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Camera")
	bool bSelectionModeActive = false;

	/** Сколько клеток сейчас в выделении (см. SelectedCells) - HUD'у, чтобы
	 *  показывать, есть ли что извлекать/удалять, и сколько именно. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Camera")
	int32 SelectedCellCount = 0;
};

/** Генератор начального состояния: что построит Y и во что это обойдётся.
 *
 *  Отдельно от симуляции, хотя и рядом по смыслу: это описание того, чего ещё
 *  НЕ произошло, - ответ на вопрос "что будет, если нажать", а не "что сейчас
 *  считается". */
USTRUCT(BlueprintType)
struct FHudGeneratorStats
{
	GENERATED_BODY()

	/** Выбранный генератор начального состояния - что построит Y (см.
	 *  StateGenerators::GetDisplayName()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Generator")
	FString StateGeneratorName;

	/** Сколько клеток он даст - оценка без построения, чтобы было видно ДО
	 *  нажатия, во что обойдётся нажатие.
	 *
	 *  Пересчитывается не каждый кадр, а раз в четверть секунды: у шаров оценка
	 *  считает решёточные точки точно, за O(R^2), и при радиусе в сотни клеток
	 *  это миллионы операций (см. UpdateHudStats()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Generator")
	int64 EstimatedGeneratorCells = 0;
};

/** Озвучивание (P) - см. docs/sonification.md. */
USTRUCT(BlueprintType)
struct FHudSonificationStats
{
	GENERATED_BODY()

	/** Озвучивание симуляции включено (P). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Sonification")
	bool bSonificationEnabled = false;

	/** Применённый набор настроек (Shift+P). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Sonification")
	FString SonificationPresetName;

	/** Форма кривой населения словом - "Взрывной рост", "Насыщение", "Обвал".
	 *
	 *  Это то же измерение, которым ведётся звук, только выведенное текстом:
	 *  услышать разницу между разгоном и насыщением можно сразу, а вот
	 *  убедиться, что подсистема поняла её так же, как ухо, - только глазами.
	 *  Пусто, когда звук выключен. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD|Sonification")
	FString SonificationShapeName;
};
