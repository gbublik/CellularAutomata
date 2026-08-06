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

	/** Пикинг одиночной клетки кликом: идёт по лучу (RayOrigin/RayDirection -
	 *  депроецированный курсор мыши) через клеточную решётку алгоритмом
	 *  Amanatides-Woo (voxel DDA - шаг ровно по границам клеток, ни одна
	 *  клетка на пути луча не пропускается и не проверяется дважды) и
	 *  возвращает ПЕРВУЮ живую клетку, т.е. ровно ту, чью грань пользователь
	 *  видит под курсором. Трассировка движка не подходит - у всех клеточных
	 *  компонентов коллизия выключена (NoCollision, см. конструктор
	 *  AAutomataOrchestrator). MaxDistance (в мировых единицах) ограничивает
	 *  обход - вызывающая сторона передаёт дистанцию до дальнего края AABB
	 *  живых клеток (см. AAutomataOrchestrator::SelectCellUnderCursor()).
	 *  false - на пути луча живых клеток нет. */
	CELLULARAUTOMATA_API bool PickCellAlongRay(
		const FCellGrid& Grid,
		const FVector& RayOrigin,
		const FVector& RayDirection,
		double MaxDistance,
		FIntVector& OutCell);

	/** То же, но с НОРМАЛЬЮ ГРАНИ, через которую луч вошёл в найденную клетку, -
	 *  единичный вектор по одной оси, вроде (0,0,1). Это то, чем клетка
	 *  "прилипает" к грани при рисовании: новая клетка ставится в
	 *  `OutCell + OutFaceNormal` (см. AAutomataOrchestrator::
	 *  ComputePlacementCell()).
	 *
	 *  Нормаль достаётся бесплатно: Amanatides-Woo шагает ровно по одной оси за
	 *  итерацию, так что ось последнего шага и есть грань входа - никакой
	 *  отдельной геометрии пересечения не считается.
	 *
	 *  Нулевая нормаль - ЗНАЧИМЫЙ ответ, а не ошибка: он означает, что луч
	 *  начался уже внутри найденной клетки (камера залетела внутрь структуры),
	 *  и грани входа не существует. Прилипать в этом случае не к чему, и
	 *  вызывающий решает сам, что делать. */
	CELLULARAUTOMATA_API bool PickCellAlongRay(
		const FCellGrid& Grid,
		const FVector& RayOrigin,
		const FVector& RayDirection,
		double MaxDistance,
		FIntVector& OutCell,
		FIntVector& OutFaceNormal);
}
