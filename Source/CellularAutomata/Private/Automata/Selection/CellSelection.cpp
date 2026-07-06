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
	const double CellSize = Grid.GetCellSize();
	if (CellSize <= 0.0 || MaxDistance <= 0.0)
	{
		return false;
	}

	const FVector Direction = RayDirection.GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		return false;
	}

	// Переходим в клеточное пространство: GridToWorld() по умолчанию даёт
	// Cell * CellSize как ЦЕНТР клетки, т.е. клетка i занимает
	// [i - 0.5, i + 0.5) в этих координатах - границы клеток на полуцелых.
	const FVector Position = RayOrigin / CellSize;

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

	// T - в клеточных единицах (Direction нормализован, Position поделён на
	// CellSize), поэтому и лимит переводим в клетки.
	const double MaxT = MaxDistance / CellSize;
	double T = 0.0;

	while (T <= MaxT)
	{
		if (Grid.IsAlive(Cell))
		{
			OutCell = Cell;
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
	}

	return false;
}
