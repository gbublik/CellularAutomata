#include "Automata/Simulation/CellAging.h"
#include "Automata/Grid/CellGrid.h"
#include "Async/ParallelFor.h"

void CellAging::ComputeAges(const FCellGrid* OldGrid, FCellGrid& NewGrid)
{
	TArray<FIntVector> NewAliveCells;
	NewGrid.GetAliveCells(NewAliveCells);

	// В отличие от SetAlive/WriteBack (последовательные - TBitArray не
	// потокобезопасен для конкурентной записи бит), SetAge пишет отдельные
	// байты по различным индексам одного чанка - это безопасно звать прямо
	// из ParallelFor, без отдельной последовательной фазы записи.
	ParallelFor(NewAliveCells.Num(), [OldGrid, &NewGrid, &NewAliveCells](int32 Index)
	{
		const FIntVector& Cell = NewAliveCells[Index];
		const uint8 Age = (OldGrid && OldGrid->IsAlive(Cell))
			? static_cast<uint8>(FMath::Min(OldGrid->GetAge(Cell) + 1, 255))
			: 0;
		NewGrid.SetAge(Cell, Age);
	});
}
