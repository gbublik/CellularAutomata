#pragma once

#include "CoreMinimal.h"
#include "Automata/Generation/CellArrayParams.h"

/** Тираж набора клеток по трём осям - модификатор Array из Blender, только над
 *  клетками. Plain namespace, не UObject: та же идиома, что StateGenerators/
 *  CellAging/CellSelection.
 *
 *  Отличие от StateGenerators, из-за которого это отдельный модуль, а не ещё
 *  один EStateGeneratorType: генератор строит набор ИЗ НИЧЕГО и по построению
 *  не читает состояние сцены, а тираж работает над уже существующим набором.
 *  Общее у них - выход: массив клеток, который заливает
 *  AAutomataOrchestrator::RebuildGridFromCells(). FCellGrid здесь так же не
 *  фигурирует, и заголовку не нужен даже его forward-declaration.
 *
 *  Дубликаты НЕ отсеиваются - см. Tile(). */
namespace CellArrayModifier
{
	/** Габарит набора в клетках: Max - Min + 1 по каждой оси (то есть у набора
	 *  из одной клетки габарит (1,1,1), а не ноль - это размер, а не расстояние).
	 *  Пустой набор даёт нули. */
	CELLULARAUTOMATA_API FIntVector ComputeSize(const TArray<FIntVector>& Cells);

	/** Шаг между соседними копиями по каждой оси, в клетках:
	 *  `round(SourceSize * RelativeOffset) + ConstantOffset`.
	 *
	 *  Отдельная функция, а не деталь Tile(), потому что вызывающему он нужен и
	 *  сам по себе: нулевой шаг при Count > 1 означает "все копии лягут одна в
	 *  одну" (результат совпадёт с источником, и об этом стоит предупредить), а
	 *  нечётный - что ГЦК/ОЦК-набор размажется по обеим подрешёткам (см.
	 *  ECellParityFilter). Ни то, ни другое не ошибка, и решает это вызывающий. */
	CELLULARAUTOMATA_API FIntVector ComputeStep(const FIntVector& SourceSize, const FCellArrayParams& Params);

	/** Верхняя оценка числа клеток на выходе - `SourceCellCount * Count.X *
	 *  Count.Y * Count.Z`, O(1), для проверки бюджета ДО построения (та же
	 *  идиома, что StateGenerators::EstimateCellCount()). Именно ВЕРХНЯЯ:
	 *  наезжающие копии дают дубликаты, которые схлопнутся при заливке. Считается
	 *  в int64 - произведение трёх Count'ов на миллионы клеток переполняет int32
	 *  задолго до того, как упрётся в бюджет. */
	CELLULARAUTOMATA_API int64 EstimateCellCount(int64 SourceCellCount, const FCellArrayParams& Params);

	/** Строит тираж: копия (i,j,k) - это весь Source, сдвинутый на
	 *  (i*Шаг.X, j*Шаг.Y, k*Шаг.Z), плюс общий сдвиг центрирования, если
	 *  включён bCenterOnSource.
	 *
	 *  Дубликаты НЕ отсеиваются, и это осознанно: при наезжающих копиях TSet на
	 *  миллионы клеток стоил бы сотни мегабайт и целый проход, а заливка
	 *  FCellGrid::SetAlive() идемпотентна - RebuildGridFromCells() всё равно
	 *  собирает InitialStateCells обратно из сетки, где дубликаты уже схлопнулись
	 *  (ровно как у RandomBall, чьи броски штатно попадают в одну клетку дважды).
	 *
	 *  Пустой Source даёт пустой результат, а не набор из нулей. */
	CELLULARAUTOMATA_API void Tile(const TArray<FIntVector>& Source, const FCellArrayParams& Params,
								   TArray<FIntVector>& OutCells);
}
