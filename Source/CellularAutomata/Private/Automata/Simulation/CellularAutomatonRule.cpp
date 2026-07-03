#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Grid/CellGrid.h"

TArray<FIntVector> FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood InNeighborhood)
{
	TArray<FIntVector> Offsets;

	if (InNeighborhood == ENeighborhood::VonNeumann)
	{
		Offsets.Reserve(6);
		Offsets.Add(FIntVector(1, 0, 0));
		Offsets.Add(FIntVector(-1, 0, 0));
		Offsets.Add(FIntVector(0, 1, 0));
		Offsets.Add(FIntVector(0, -1, 0));
		Offsets.Add(FIntVector(0, 0, 1));
		Offsets.Add(FIntVector(0, 0, -1));
		return Offsets;
	}

	// Moore: полный куб 3x3x3 без центра (26 соседей)
	Offsets.Reserve(26);
	for (int32 dx = -1; dx <= 1; ++dx)
	{
		for (int32 dy = -1; dy <= 1; ++dy)
		{
			for (int32 dz = -1; dz <= 1; ++dz)
			{
				if (dx == 0 && dy == 0 && dz == 0)
				{
					continue;
				}
				Offsets.Add(FIntVector(dx, dy, dz));
			}
		}
	}
	return Offsets;
}

FCellularAutomatonRule::FCellularAutomatonRule(const TArray<int32>& InBirthCounts, const TArray<int32>& InSurvivalCounts, ENeighborhood InNeighborhood)
	: NeighborOffsets(BuildNeighborOffsets(InNeighborhood))
	, BirthCounts(InBirthCounts)
	, SurvivalCounts(InSurvivalCounts)
{
}

void FCellularAutomatonRule::Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid) const
{
	TArray<FIntVector> AliveCells;
	CurrentGrid.GetAliveCells(AliveCells);

	TSet<FIntVector> Candidates;
	Candidates.Reserve(AliveCells.Num() * (NeighborOffsets.Num() + 1));
	for (const FIntVector& Cell : AliveCells)
	{
		Candidates.Add(Cell);
		for (const FIntVector& Offset : NeighborOffsets)
		{
			Candidates.Add(Cell + Offset);
		}
	}

	for (const FIntVector& Candidate : Candidates)
	{
		int32 AliveNeighborCount = 0;
		for (const FIntVector& Offset : NeighborOffsets)
		{
			if (CurrentGrid.IsAlive(Candidate + Offset))
			{
				++AliveNeighborCount;
			}
		}

		const bool bCurrentlyAlive = CurrentGrid.IsAlive(Candidate);
		const bool bNextAlive = bCurrentlyAlive
			? SurvivalCounts.Contains(AliveNeighborCount)
			: BirthCounts.Contains(AliveNeighborCount);

		if (bNextAlive)
		{
			NextGrid.SetAlive(Candidate, true);
		}
	}
}
