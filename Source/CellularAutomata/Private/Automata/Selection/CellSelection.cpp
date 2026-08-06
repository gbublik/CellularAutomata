#include "Automata/Selection/CellSelection.h"
#include "Automata/Grid/CellGrid.h"
#include "Async/ParallelFor.h"

TArray<FIntVector> CellSelection::SelectCellsInScreenRect(
	const FCellGrid& Grid,
	const FMatrix& ViewProjectionMatrix,
	const FVector2D& ViewportSize,
	const FVector2D& RectMin,
	const FVector2D& RectMax)
{
	TArray<FIntVector> AliveCells;
	Grid.GetAliveCells(AliveCells);

	// Двухфазная схема, как и CellAging::ComputeAges()/FCpuComputeStrategy:
	// параллельно считаем булев флаг "попала в прямоугольник" по индексу
	// (запись в TArray<bool> по различным индексам потокобезопасна), затем
	// один последовательный проход собирает итоговый список - сама TArray<>
	// не потокобезопасна для конкурентного Add().
	TArray<bool> InRect;
	InRect.SetNumZeroed(AliveCells.Num());

	ParallelFor(AliveCells.Num(), [&Grid, &ViewProjectionMatrix, &ViewportSize, &RectMin, &RectMax, &AliveCells, &InRect](int32 Index)
	{
		const FVector WorldPos = Grid.GridToWorld(AliveCells[Index]);
		const FVector4 Projected = ViewProjectionMatrix.TransformFVector4(FVector4(WorldPos, 1.0));
		if (Projected.W <= KINDA_SMALL_NUMBER)
		{
			// За камерой - не может попасть в экранный прямоугольник.
			return;
		}

		const float NdcX = Projected.X / Projected.W;
		const float NdcY = Projected.Y / Projected.W;
		const float ScreenX = (NdcX * 0.5f + 0.5f) * ViewportSize.X;
		const float ScreenY = (1.0f - (NdcY * 0.5f + 0.5f)) * ViewportSize.Y;

		InRect[Index] = (ScreenX >= RectMin.X) && (ScreenX <= RectMax.X)
			&& (ScreenY >= RectMin.Y) && (ScreenY <= RectMax.Y);
	});

	TArray<FIntVector> Result;
	Result.Reserve(AliveCells.Num());
	for (int32 Index = 0; Index < AliveCells.Num(); ++Index)
	{
		if (InRect[Index])
		{
			Result.Add(AliveCells[Index]);
		}
	}
	return Result;
}

bool CellSelection::PickCellAlongRay(
	const FCellGrid& Grid,
	const FVector& RayOrigin,
	const FVector& RayDirection,
	double MaxDistance,
	FIntVector& OutCell)
{
	FIntVector UnusedNormal;
	return PickCellAlongRay(Grid, RayOrigin, RayDirection, MaxDistance, OutCell, UnusedNormal);
}

bool CellSelection::PickCellAlongRay(
	const FCellGrid& Grid,
	const FVector& RayOrigin,
	const FVector& RayDirection,
	double MaxDistance,
	FIntVector& OutCell,
	FIntVector& OutFaceNormal)
{
	OutFaceNormal = FIntVector::ZeroValue;

	const FLatticeTransform& Lattice = Grid.GetLattice();
	const FVector CellStep = Lattice.GetCellWorldExtent();
	if (CellStep.GetMin() <= 0.0 || MaxDistance <= 0.0)
	{
		return false;
	}

	const FVector WorldDirection = RayDirection.GetSafeNormal();
	if (WorldDirection.IsNearlyZero())
	{
		return false;
	}

	// Переходим в клеточное пространство: GridToWorld() даёт ЦЕНТР клетки,
	// т.е. клетка i занимает [i - 0.5, i + 0.5) в этих координатах - границы
	// клеток на полуцелых, на чём и стоит весь Amanatides-Woo ниже.
	const FVector Position = Lattice.WorldToGridFractional(RayOrigin);

	// Направление переводим ПОКОМПОНЕНТНЫМ делением на шаг решётки и
	// СОЗНАТЕЛЬНО не нормируем заново. Мировое направление уже единичное,
	// поэтому пройденный параметр T остаётся в МИРОВЫХ единицах, и лимит
	// дальности берётся как есть (раньше он делился на CellSize). На решётке
	// с неравным шагом по осям это единственный способ сохранить смысл
	// MaxDistance: нормировка в индексном пространстве растянула бы его по
	// каждой оси по-своему.
	const FVector Direction = WorldDirection / CellStep;

	FIntVector Cell(
		FMath::RoundToInt(Position.X),
		FMath::RoundToInt(Position.Y),
		FMath::RoundToInt(Position.Z));

	// Классический Amanatides-Woo: для каждой оси - расстояние вдоль луча до
	// ближайшего пересечения границы клетки (TMax) и шаг расстояния между
	// последовательными границами этой оси (TDelta). На каждой итерации
	// продвигаемся по оси с наименьшим TMax.
	FIntVector StepSign(0, 0, 0);
	FVector TMax(TNumericLimits<double>::Max(), TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	FVector TDelta(TNumericLimits<double>::Max(), TNumericLimits<double>::Max(), TNumericLimits<double>::Max());

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (FMath::IsNearlyZero(Direction[Axis]))
		{
			continue;
		}

		StepSign[Axis] = Direction[Axis] > 0.0 ? 1 : -1;
		const double NextBoundary = Cell[Axis] + StepSign[Axis] * 0.5;
		TMax[Axis] = (NextBoundary - Position[Axis]) / Direction[Axis];
		TDelta[Axis] = 1.0 / FMath::Abs(Direction[Axis]);
	}

	// T - уже в мировых единицах: Direction поделён на шаг решётки ровно
	// затем, чтобы лимит не пришлось переводить (см. выше).
	const double MaxT = MaxDistance;
	double T = 0.0;

	// Ось последнего шага - это и есть грань, через которую луч вошёл в клетку:
	// Amanatides-Woo шагает ровно по одной оси за раз, так что нормаль входа
	// известна бесплатно, отдельной геометрии не нужно. -1 означает "шага ещё не
	// было", то есть луч начался внутри найденной клетки (камера внутри
	// структуры) - грани входа тогда не существует, и нормаль остаётся нулевой.
	int32 LastStepAxis = -1;

	while (T <= MaxT)
	{
		if (Grid.IsAlive(Cell))
		{
			OutCell = Cell;
			if (LastStepAxis >= 0)
			{
				// Против направления шага: шагали в +X - вошли через грань,
				// смотрящую в -X.
				OutFaceNormal[LastStepAxis] = -StepSign[LastStepAxis];
			}
			return true;
		}

		int32 MinAxis = 0;
		if (TMax.Y < TMax[MinAxis])
		{
			MinAxis = 1;
		}
		if (TMax.Z < TMax[MinAxis])
		{
			MinAxis = 2;
		}

		T = TMax[MinAxis];
		TMax[MinAxis] += TDelta[MinAxis];
		Cell[MinAxis] += StepSign[MinAxis];
		LastStepAxis = MinAxis;
	}

	return false;
}
