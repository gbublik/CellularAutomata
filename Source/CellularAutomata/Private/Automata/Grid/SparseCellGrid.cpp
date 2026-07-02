#include "Automata/Grid/SparseCellGrid.h"

FSparseCellGrid::FSparseCellGrid(float InCellSize)
	: FCellGrid(InCellSize)
{
}

bool FSparseCellGrid::IsAlive(const FIntVector& Cell) const
{
	return AliveCells.Contains(Cell);
}

void FSparseCellGrid::SetAlive(const FIntVector& Cell, bool bAlive)
{
	if (bAlive)
	{
		AliveCells.Add(Cell);
	}
	else
	{
		AliveCells.Remove(Cell);
	}
}

void FSparseCellGrid::Clear()
{
	AliveCells.Empty();
}

int32 FSparseCellGrid::Num() const
{
	return AliveCells.Num();
}

void FSparseCellGrid::GetAliveCells(TArray<FIntVector>& OutCells) const
{
	OutCells.Reset(AliveCells.Num());
	for (const FIntVector& Cell : AliveCells)
	{
		OutCells.Add(Cell);
	}
}
