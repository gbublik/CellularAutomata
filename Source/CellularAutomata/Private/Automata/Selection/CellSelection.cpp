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
