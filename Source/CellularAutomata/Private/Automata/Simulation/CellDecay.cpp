#include "Automata/Simulation/CellDecay.h"
#include "Automata/Grid/CellGrid.h"

void CellDecay::AdvanceDecayStates(const FCellGrid* OldGrid, FCellGrid& NextGrid, int32 MaxStates)
{
	if (MaxStates <= 2 || !OldGrid)
	{
		return;
	}

	// Случай А - была жива, не выжила: начинает угасание с состояния 2.
	// Клетки, что выжили (IsAlive(Cell) в NextGrid true), этим циклом не
	// затрагиваются.
	TArray<FIntVector> OldAliveCells;
	OldGrid->GetAliveCells(OldAliveCells);
	for (const FIntVector& Cell : OldAliveCells)
	{
		if (!NextGrid.IsAlive(Cell))
		{
			NextGrid.SetDecayState(Cell, 2);
		}
	}

	// Случай Б - уже угасала: состояние растёт на 1, пока не достигнет
	// MaxStates - тогда клетка окончательно умирает (запись не делается,
	// 0 = отсутствие записи).
	TArray<FIntVector> OldDecayingCells;
	TArray<uint8> OldDecayingStates;
	OldGrid->GetDecayingCells(OldDecayingCells, OldDecayingStates);
	for (int32 Index = 0; Index < OldDecayingCells.Num(); ++Index)
	{
		const FIntVector& Cell = OldDecayingCells[Index];

		// Защитный пропуск - при корректной последовательности birth-immunity
		// (см. FCpuComputeStrategy::Step()/.usf) угасающая клетка не должна
		// оказаться живой в NextGrid, но дешевле проверить, чем молча
		// перезаписать её состояние угасания поверх настоящего рождения.
		if (NextGrid.IsAlive(Cell))
		{
			continue;
		}

		const int32 NewState = static_cast<int32>(OldDecayingStates[Index]) + 1;
		if (NewState < MaxStates)
		{
			NextGrid.SetDecayState(Cell, static_cast<uint8>(NewState));
		}
		// NewState == MaxStates - окончательно мертва, ничего не пишем.
	}
}
