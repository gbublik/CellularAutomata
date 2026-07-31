#pragma once

#include "CoreMinimal.h"
#include "GenerationHistory.generated.h"

/** Одна точка графика поколений: сколько клеток было живо во всей сетке и
 *  сколько реально уехало в AddInstances() после всех фильтров (куб отсечения,
 *  фильтр по возрасту, срез).
 *
 *  Хранится ПАРОЙ с номером поколения, а не одним значением с неявным X по
 *  индексу массива, и это принципиально: один заход StepAsync() при GPU-батчинге
 *  считает сразу пачку поколений (см. AAutomataOrchestrator::
 *  LastDispatchGenerations), причём размер пачки подрезается на лету по объёму
 *  AABB. Замеры в окне стоят неравномерно, и раскладка по индексу сжимала бы и
 *  растягивала кривую при каждой смене размера пачки.
 *
 *  ВНИМАНИЕ: RenderedCount может ЗАКОННО превышать AliveCount - при правилах
 *  Generations (States > 2) угасающие клетки рисуются, но живыми не считаются
 *  (см. doc-comment FCellRenderStats). Ничто в масштабировании графика не имеет
 *  права считать одну линию заведомо выше другой. */
USTRUCT(BlueprintType)
struct FGenerationSample
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int64 Generation = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 AliveCount = 0;

	/** Может быть ПЕРЕНЕСЁННЫМ с предыдущего замера, а не измеренным на этом
	 *  поколении - см. Append(). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 RenderedCount = 0;
};

/** Скользящее окно последних Capacity ЗАМЕРОВ для графика поколений на HUD -
 *  первое в проекте хранилище истории (всё остальное - FRenderTimings,
 *  FCellRenderStats, FHudStats - снимки последнего момента).
 *
 *  Свободные функции над чужим массивом, без UObject и без актора - тот же
 *  идиом, что CellAging/CellSelection/CellMeshBuilder, и по той же причине:
 *  вся неочевидная логика (перенос значения, срез по ёмкости, правка последнего
 *  замера на месте) покрывается автотестами headless, без PIE и рендера.
 *  Владеет массивом AAutomataOrchestrator (GenerationSamples), здесь только
 *  правила работы с ним.
 *
 *  Ёмкость считается в ЗАМЕРАХ, а не в поколениях: один заход может посчитать
 *  пачку, так что в поколениях окно шире ровно на её размер.
 *
 *  Потокобезопасности нет и не требуется: все записи идут с игрового потока
 *  (продолжения AsyncTask(GameThread) в ApplyStepResult()/Next() и функции
 *  рендера), отрисовка виджета - тоже. */
namespace GenerationHistory
{
	/** Замер на новое поколение.
	 *
	 *  RenderedCount ПЕРЕНОСИТСЯ с предыдущего замера (0, если истории нет):
	 *  при StepsPerRender > 1 и GPU-батчинге большинство поколений на экран не
	 *  попадают вовсе, и линия "видимо" держит последнее известное значение
	 *  ступенькой. Альтернатива - разрывы в линии - при StepsPerRender 256
	 *  оставила бы от неё редкие точки. */
	CELLULARAUTOMATA_API void Append(TArray<FGenerationSample>& History,
		int64 Generation, int32 AliveCount, int32 Capacity);

	/** Сколько инстансов реально ушло в AddInstances().
	 *
	 *  Если последний замер - про ЭТО ЖЕ поколение, правит его НА МЕСТЕ, а не
	 *  добавляет новый. Это несущая часть, а не оптимизация: рендер зовётся и
	 *  без смены поколения - RefreshRenderCullVolume() дёргается из Tick() на
	 *  каждое движение камеры при включённом срезе, DeleteSelectedCells()
	 *  правит сетку прямо на паузе. Без правки на месте одно поколение
	 *  размазалось бы на сотни точек по X от одного полёта камеры.
	 *
	 *  Если истории нет или последний замер про другое поколение - добавляет
	 *  замер. Так засевается поколение 0 сразу после ResetGenerationCounter():
	 *  все пять путей "начать прогон заново" чистят историю и тут же рисуют. */
	CELLULARAUTOMATA_API void NoteRendered(TArray<FGenerationSample>& History,
		int64 Generation, int32 AliveCount, int32 RenderedCount, int32 Capacity);

	/** Границы окна для масштабирования. false, если история пуста (рисовать
	 *  нечего). OutMaxY - максимум по ОБОИМ рядам, см. FGenerationSample. */
	CELLULARAUTOMATA_API bool ComputeBounds(const TArray<FGenerationSample>& History,
		int64& OutMinGeneration, int64& OutMaxGeneration, int32& OutMaxY);

	/** Замеры -> точки в локальных координатах виджета (начало - левый верхний
	 *  угол, Y вниз, поэтому значение инвертируется). Чистая математика на
	 *  FVector2f, без Slate - тестируется headless вместе с остальным.
	 *
	 *  MaxY - уже сглаженный "красивый" потолок, а не сырой максимум из
	 *  ComputeBounds(): пересчёт сырого максимума на каждом кадре заставляет
	 *  всю кривую непрерывно дышать. Значения выше MaxY подрезаются. */
	CELLULARAUTOMATA_API void MapToPoints(const TArray<FGenerationSample>& History,
		const FVector2f& PlotSize, const FVector2f& PlotOrigin,
		int64 MinGeneration, int64 MaxGeneration, double MaxY, bool bLogScale,
		TArray<FVector2f>& OutAlivePoints, TArray<FVector2f>& OutRenderedPoints);

	/** Ближайший сверху "красивый" потолок (1/2/5 * 10^k) - чтобы подпись оси
	 *  читалась и чтобы масштаб не менялся от каждой мелкой ряби. */
	CELLULARAUTOMATA_API double NiceCeiling(double Value);
}
