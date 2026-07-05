#pragma once

#include "CoreMinimal.h"

class FCellGrid;

/** Выбор живых клеток, чья экранная проекция попадает в прямоугольник
 *  выделения (marquee) - отдельный проход, не часть рендера/симуляции,
 *  тот же namespace-стиль, что и CellAging. */
namespace CellSelection
{
	/** Возвращает все живые клетки Grid, чья проекция (через
	 *  ViewProjectionMatrix, посчитанную один раз на всю операцию, не на
	 *  клетку) попадает в прямоугольник [RectMin, RectMax] (включительно,
	 *  экранные пиксели viewport размера ViewportSize). Проверка глубины не
	 *  делается - выделение работает как бесконечная призма вдоль взгляда
	 *  камеры; клетки позади камеры (W <= 0 после проекции) исключаются
	 *  автоматически, т.к. не проходят дальнейшую проверку. Параллелится по
	 *  живым клеткам (ParallelFor), т.к. может понадобиться на миллионах
	 *  клеток как разовое действие по отпусканию мыши. */
	CELLULARAUTOMATA_API TArray<FIntVector> SelectCellsInScreenRect(
		const FCellGrid& Grid,
		const FMatrix& ViewProjectionMatrix,
		const FVector2D& ViewportSize,
		const FVector2D& RectMin,
		const FVector2D& RectMax);
}
