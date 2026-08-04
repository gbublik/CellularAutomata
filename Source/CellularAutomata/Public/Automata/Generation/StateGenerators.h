#pragma once

#include "CoreMinimal.h"
#include "Automata/Generation/StateGeneratorParams.h"
#include "Automata/Simulation/Neighborhood.h"

/** Геометрические генераторы начального состояния - решётки, тела, шум,
 *  симметричные затравки. Plain namespace-функции, не UObject - та же идиома,
 *  что CellAging/CellSelection/CellMeshBuilder.
 *
 *  Генератор ВОЗВРАЩАЕТ МАССИВ КЛЕТОК, а не заполняет FCellGrid, и это
 *  принципиально: FCellGrid::SetAlive() не потокобезопасен (кеширует последний
 *  чанк), так что заливка обязана быть последовательной и жить в одном месте
 *  (AAutomataOrchestrator::RebuildGridFromCells()). Зато сами генераторы
 *  свободно параллелятся, число клеток можно посчитать заранее, а весь модуль
 *  тестируется без движка - заголовку не нужен даже forward-declaration сетки.
 *
 *  Все генераторы строят область, ЦЕНТРИРОВАННУЮ В НУЛЕ (координаты клеток
 *  штатно отрицательные - см. FDenseCellGrid), и детерминированы: тот же Seed
 *  даёт тот же результат бит в бит. */
namespace StateGenerators
{
	/** Во что обошлась генерация - для лога и для HUD. */
	struct FGenerateStats
	{
		/** Время работы Generate() в секундах. */
		double Seconds = 0.0;

		/** Сколько клеток попало в результат (уже после дедупликации там, где
		 *  она делается). */
		int64 EmittedCells = 0;

		/** Сколько клеток пришлось перебрать. У конструктивных генераторов
		 *  (решётки, затравки) совпадает с EmittedCells, у сканирующих (тела,
		 *  шум) равно объёму области - именно этим временем, а не выходом,
		 *  определяется, сколько генератор будет думать. */
		int64 ScannedCells = 0;
	};

	/** Распределение числа живых соседей - см.
	 *  FStateGeneratorParams::bAnalyzeNeighborCounts. Длина обеих таблиц -
	 *  "число соседей + 1" для запрошенной пары (соседство, радиус), то есть
	 *  27 для привычного Moore-26; индекс - число живых соседей. */
	struct FNeighborHistogram
	{
		/** Сколько живых клеток видит ровно N живых соседей. */
		TArray<int64> AliveByCount;

		/** Сколько ПУСТЫХ клеток, примыкающих хотя бы к одной живой, видит
		 *  ровно N живых соседей. */
		TArray<int64> EmptyByCount;

		/** Сколько клеток попало в выборку (структуры периодичны, считается
		 *  центральный подкуб, а не весь набор). */
		int64 SampledAlive = 0;
		int64 SampledEmpty = 0;
	};

	/** Строит набор клеток по параметрам.
	 *
	 *  Возвращает true и заполняет OutCells/OutStats; при ошибке возвращает
	 *  false, заполняет OutError человекочитаемым описанием и НЕ ТРОГАЕТ
	 *  OutCells вовсе - генератор собирает результат во внутренний массив и
	 *  отдаёт его только при успехе, так что неудачная генерация не может
	 *  оставить вызывающего с полу-построенным состоянием.
	 *
	 *  MaxCells - потолок на число клеток. Оценка EstimateCellCount() у
	 *  шумовых генераторов приблизительная, поэтому предел проверяется ещё и
	 *  по факту в ходе работы: как только выход его перевалил, генератор
	 *  бросает работу и возвращает false. Передать MAX_int64 - значит не
	 *  ограничивать (так делает путь GenerateRandom(), который исторически
	 *  ничего не ограничивал). */
	CELLULARAUTOMATA_API bool Generate(const FStateGeneratorParams& Params, int32 Seed, int64 MaxCells,
									   TArray<FIntVector>& OutCells, FGenerateStats& OutStats, FString& OutError);

	/** Оценка числа клеток на выходе, O(1), без единой аллокации - вызывающий
	 *  проверяет бюджет ДО начала работы (та же идиома, что
	 *  CellMeshBuilder::CountExposedFaces() перед бейком), а сам генератор
	 *  берёт по ней Reserve.
	 *
	 *  Оценка ВЕРХНЯЯ там, где возможны наложения (пересечения плит и балок,
	 *  клетки на плоскостях симметрии, перекрытие зёрен), и ожидаемая (а не
	 *  гарантированная) у шума. */
	CELLULARAUTOMATA_API int64 EstimateCellCount(const FStateGeneratorParams& Params);

	/** Оценка числа клеток, которые придётся перебрать - см.
	 *  FGenerateStats::ScannedCells. */
	CELLULARAUTOMATA_API int64 EstimateScannedCells(const FStateGeneratorParams& Params);

	/** Отображаемое имя генератора - для лога, HUD и экранного сообщения. */
	CELLULARAUTOMATA_API FString GetDisplayName(EStateGeneratorType Type);

	/** Считает гистограмму соседей по центральному подкубу набора.
	 *
	 *  MaxSampleExtent ограничивает полуразмер выборки (структуры периодичны,
	 *  так что ответ от этого не меняется, а TSet на миллионы клеток стоил бы
	 *  сотни мегабайт). Соседство берётся то же, по которому считает
	 *  симуляция, - это геометрия набора, а не правило: ни BirthCounts, ни
	 *  SurvivalCounts здесь не участвуют. */
	/** То же, но соседство задано СПИСКОМ смещений - для наборов, не
	 *  выражаемых оболочками (см. ELatticeNeighborhood). Версия от
	 *  ENeighborhood делегирует сюда, так что обе меряют одним кодом. */
	CELLULARAUTOMATA_API void AnalyzeNeighborCounts(const TArray<FIntVector>& Cells, const TArray<FIntVector>& Offsets,
													int32 MaxSampleExtent, FNeighborHistogram& OutHistogram);

	CELLULARAUTOMATA_API void AnalyzeNeighborCounts(const TArray<FIntVector>& Cells, ENeighborhood Neighborhood,
													int32 MaxSampleExtent, FNeighborHistogram& OutHistogram);

	/** Гистограмма одной строкой для лога: перечисляет только ненулевые
	 *  колонки, отдельно живые и примыкающие пустые. */
	CELLULARAUTOMATA_API FString DescribeHistogram(const FNeighborHistogram& Histogram);
}
