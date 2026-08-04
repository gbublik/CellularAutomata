#pragma once

#include "CoreMinimal.h"

/** Отображение целых координат клетки в мир и обратно - вся геометрия
 *  решётки, собранная в одно значение.
 *
 *  ЗАЧЕМ ОТДЕЛЬНЫЙ ТИП. До него геометрия жила одним числом (float CellSize
 *  в FCellGrid) и формулой Cell * CellSize, размазанной по проекту: прямое
 *  преобразование было одно (FCellGrid::GridToWorld()), а ОБРАТНОГО не
 *  существовало вовсе - вместо него в четырёх местах стояло написанное от
 *  руки деление на CellSize (отбраковка чанков в FDenseCellGrid, DDA-пик в
 *  CellSelection, проекция клетки в пиксель в CellRasterizer, сдвиг куба
 *  отсечения в оркестраторе). Пока решётка кубическая, все четыре верны и
 *  расхождение незаметно; на решётке с неравным шагом по осям каждое из них
 *  ломается по-своему и молча. Поэтому поле CellSize из FCellGrid УДАЛЕНО, а
 *  не дополнено: так каждое такое место стало ошибкой компиляции, а не
 *  потенциальным пятым экземпляром той же формулы.
 *
 *  ПОЧЕМУ POD, А НЕ ИЕРАРХИЯ. В проекте сменное поведение принято оформлять
 *  абстрактным классом с TUniquePtr (FCellularAutomatonComputeStrategy,
 *  FCellGridRenderer, сам FCellGrid). Здесь это было бы ошибкой: те зовут по
 *  разу за шаг или за рендер, а это - миллионы раз за кадр
 *  (AAutomataOrchestrator::BuildCellRenderData(), ComputeCellsBounds()).
 *  Значение копируется по 56 байт, разворачивается инлайном и не стоит ни
 *  одного виртуального вызова; ровно за счёт этого переход на решётку с
 *  неравным шагом не только не замедлил рендер, но и ускорил его - там, где
 *  раньше был virtual GridToWorld(), теперь инлайновое умножение.
 *
 *  ЧТО СЮДА НЕ ВХОДИТ. Только отображение координат - и ничего о том, какая
 *  это фигура. Габарит ячейки Вороного, набор соседей, фильтр чётности и меш
 *  живут в пресете формы (CellShapePresets.h), потому что решётка про фигуру
 *  не знает: GridToWorld() у простой кубической, ГЦК и ОЦК - буквально одна
 *  и та же функция, а различаются они тем, какие узлы Z^3 заселены и какие
 *  считаются соседями. */
struct CELLULARAUTOMATA_API FLatticeTransform
{
	/** Мировой шаг на единицу индекса по каждой оси. Равные компоненты -
	 *  привычная кубическая решётка (куб, ГЦК, ОЦК: у всех трёх шаг
	 *  изотропен, различие только в заселённых узлах). Неравные - решётка,
	 *  растянутая по оси; на ОЦК с растяжением по Z ячейкой Вороного
	 *  становится удлинённый додекаэдр (см. CellShapePresets). */
	FVector CellWorldStep = FVector(100.0);

	/** Сдвиг начала координат. Ноль у настоящих клеток; у FChunkGridView -
	 *  (РазмерЧанка - РазмерКлетки)/2, потому что GridToWorld() даёт ЦЕНТР
	 *  ячейки, а чанк C занимает клетки от C*ChunkSize до C*ChunkSize+
	 *  ChunkSize-1 и его центр лежит на полчанка минус полклетки дальше.
	 *  Раньше эта поправка была отдельным скалярным полем и собственным
	 *  override GridToWorld() у вьюхи - теперь она просто слагаемое здесь, и
	 *  override исчез (заодно поправка стала покомпонентной, чего скаляр не
	 *  умел). */
	FVector Origin = FVector::ZeroVector;

	FLatticeTransform() = default;

	explicit FLatticeTransform(const FVector& InCellWorldStep, const FVector& InOrigin = FVector::ZeroVector)
		: CellWorldStep(InCellWorldStep)
		, Origin(InOrigin)
	{
	}

	/** Прямоугольная решётка. ZScale - растяжение по Z относительно шага в
	 *  плоскости; 1.0 даёт ровно прежнее поведение Cell * CellSize. */
	static FLatticeTransform MakeOrthogonal(float CellSize, float ZScale = 1.0f)
	{
		return FLatticeTransform(FVector(CellSize, CellSize, CellSize * ZScale));
	}

	/** Решётка в масштабе чанков - шаг ChunkSize клеток, начало сдвинуто в
	 *  центр чанка (см. Origin выше). */
	static FLatticeTransform MakeChunkView(const FLatticeTransform& CellLattice, int32 ChunkSize)
	{
		const FVector ChunkStep = CellLattice.CellWorldStep * static_cast<double>(ChunkSize);
		return FLatticeTransform(ChunkStep, (ChunkStep - CellLattice.CellWorldStep) * 0.5);
	}

	/** Огрубление в Factor раз (GridDownsample) - шаг растёт, начало
	 *  остаётся. */
	FLatticeTransform Scaled(int32 Factor) const
	{
		const int32 SafeFactor = FMath::Max(1, Factor);
		return FLatticeTransform(CellWorldStep * static_cast<double>(SafeFactor), Origin);
	}

	/** Центр клетки в мире. Именно ЦЕНТР, а не угол - на этом стоят и
	 *  полуцелые границы в DDA-пике, и поправка Origin у вьюхи чанков. */
	FORCEINLINE FVector GridToWorld(const FIntVector& Cell) const
	{
		return FVector(Cell.X, Cell.Y, Cell.Z) * CellWorldStep + Origin;
	}

	/** Разность координат в мировую разность - то же самое без Origin.
	 *  Нужно там, где переносят на сколько-то клеток, а не адресуют клетку
	 *  (сдвиг куба отсечения стрелками): Origin в разности обязан
	 *  сократиться, и отдельный метод не даёт забыть его вычесть. */
	FORCEINLINE FVector GridDeltaToWorld(const FIntVector& Delta) const
	{
		return FVector(Delta.X, Delta.Y, Delta.Z) * CellWorldStep;
	}

	/** Мировая точка в координаты клетки, которой она принадлежит - обратная
	 *  к GridToWorld(). Округление, а не floor, ровно потому, что GridToWorld
	 *  даёт центр: клетка i занимает [i - 0.5, i + 0.5) шага по каждой оси. */
	FORCEINLINE FIntVector WorldToGrid(const FVector& World) const
	{
		const FVector Local = (World - Origin) / GetSafeStep();
		return FIntVector(
			FMath::RoundToInt(Local.X),
			FMath::RoundToInt(Local.Y),
			FMath::RoundToInt(Local.Z));
	}

	/** Мировая точка в дробные координаты клетки - то же самое без
	 *  округления. Нужно трассировке луча, которая обязана знать, где внутри
	 *  клетки она находится, а не только в какой. */
	FORCEINLINE FVector WorldToGridFractional(const FVector& World) const
	{
		return (World - Origin) / GetSafeStep();
	}

	/** Клеточная рамка, ГАРАНТИРОВАННО накрывающая мировой бокс. Намеренно
	 *  консервативна (floor/ceil, то есть чуть шире точной): вызывающие -
	 *  отбраковка чанков в FDenseCellGrid - после неё всё равно проверяют
	 *  каждую клетку точно, и лишняя клетка по краю им ничего не стоит, а
	 *  потерянная была бы дыркой на границе куба отсечения. */
	FORCEINLINE void WorldBoundsToCellRange(const FBox& WorldBounds, FIntVector& OutMinCell, FIntVector& OutMaxCell) const
	{
		const FVector Step = GetSafeStep();
		const FVector Min = (WorldBounds.Min - Origin) / Step;
		const FVector Max = (WorldBounds.Max - Origin) / Step;

		OutMinCell = FIntVector(
			FMath::FloorToInt(Min.X),
			FMath::FloorToInt(Min.Y),
			FMath::FloorToInt(Min.Z));
		OutMaxCell = FIntVector(
			FMath::CeilToInt(Max.X),
			FMath::CeilToInt(Max.Y),
			FMath::CeilToInt(Max.Z));
	}

	/** Мировой габарит одной ячейки решётки по каждой оси - расстояние между
	 *  центрами соседних узлов. НЕ габарит нарисованного меша: на подрешётке
	 *  (ГЦК/ОЦК) заселён не каждый узел, и ячейка Вороного там вдвое крупнее
	 *  (см. CellMeshScaleMultiplier). */
	FORCEINLINE FVector GetCellWorldExtent() const
	{
		return CellWorldStep;
	}

	/** Шаг в плоскости - то, что исторически звалось CellSize и во что
	 *  упираются все настройки, задающие единственное число. */
	FORCEINLINE float GetPlanarCellSize() const
	{
		return static_cast<float>(CellWorldStep.X);
	}

	/** Наибольший шаг по любой оси - для радиусов и лимитов дальности, где
	 *  занижение приводит к недолёту (луч по вытянутой оси). */
	FORCEINLINE float GetMaxCellWorldExtent() const
	{
		return static_cast<float>(CellWorldStep.GetMax());
	}

	/** Одинаков ли шаг по всем трём осям. По этому признаку инструменты
	 *  решают, законна ли их кубическая арифметика: группа симметрий куба
	 *  переставляет оси X<->Z, и на растянутой решётке такая перестановка
	 *  перестаёт быть симметрией (см. ESeedSymmetry::FullCubic). */
	FORCEINLINE bool IsIsotropic() const
	{
		return FMath::IsNearlyEqual(CellWorldStep.X, CellWorldStep.Y) &&
			FMath::IsNearlyEqual(CellWorldStep.X, CellWorldStep.Z);
	}

	FORCEINLINE bool operator==(const FLatticeTransform& Other) const
	{
		return CellWorldStep.Equals(Other.CellWorldStep) && Origin.Equals(Other.Origin);
	}

	FORCEINLINE bool operator!=(const FLatticeTransform& Other) const
	{
		return !(*this == Other);
	}

private:
	/** Деление покомпонентное, поэтому нулевой шаг по любой оси дал бы
	 *  бесконечность и NaN в координате клетки - а такая клетка потом
	 *  адресует чанк и роняет хранилище далеко от места ошибки. CellSize
	 *  клампится в оркестраторе (ClampMin=1.0), так что это защита от
	 *  дефолтно-сконструированного значения, а не ожидаемый путь. */
	FORCEINLINE FVector GetSafeStep() const
	{
		return FVector(
			FMath::IsNearlyZero(CellWorldStep.X) ? 1.0 : CellWorldStep.X,
			FMath::IsNearlyZero(CellWorldStep.Y) ? 1.0 : CellWorldStep.Y,
			FMath::IsNearlyZero(CellWorldStep.Z) ? 1.0 : CellWorldStep.Z);
	}
};
