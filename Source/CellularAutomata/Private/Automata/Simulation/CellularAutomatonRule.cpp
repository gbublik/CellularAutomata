#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Grid/CellGrid.h"
#include "Async/ParallelFor.h"

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

	TSet<FIntVector> CandidateSet;
	CandidateSet.Reserve(AliveCells.Num() * (NeighborOffsets.Num() + 1));
	for (const FIntVector& Cell : AliveCells)
	{
		CandidateSet.Add(Cell);
		for (const FIntVector& Offset : NeighborOffsets)
		{
			CandidateSet.Add(Cell + Offset);
		}
	}

	// ParallelFor нужен индексируемый массив, а не TSet.
	const TArray<FIntVector> Candidates = CandidateSet.Array();

	// Подсчёт соседей на кандидата не зависит от других кандидатов (только
	// читает CurrentGrid, которую Step() не мутирует) - самая тяжёлая часть
	// шага, безопасно распараллелить по CPU-потокам. Каждый поток пишет в
	// свой индекс TArray<bool> (не бит-упакованный в Unreal, в отличие от
	// std::vector<bool> - гонок по соседним элементам нет).
	TArray<bool> bNextAlive;
	bNextAlive.SetNumUninitialized(Candidates.Num());

	ParallelFor(Candidates.Num(), [this, &Candidates, &CurrentGrid, &bNextAlive](int32 Index)
	{
		const FIntVector& Candidate = Candidates[Index];

		int32 AliveNeighborCount = 0;
		for (const FIntVector& Offset : NeighborOffsets)
		{
			if (CurrentGrid.IsAlive(Candidate + Offset))
			{
				++AliveNeighborCount;
			}
		}

		const bool bCurrentlyAlive = CurrentGrid.IsAlive(Candidate);
		bNextAlive[Index] = bCurrentlyAlive
			? SurvivalCounts.Contains(AliveNeighborCount)
			: BirthCounts.Contains(AliveNeighborCount);
	});

	// Запись в NextGrid - последовательно: TSet/TBitArray внутри
	// FSparseCellGrid/FDenseCellGrid не потокобезопасны для конкурентной
	// записи, поэтому SetAlive() нельзя звать прямо из ParallelFor-лямбды.
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		if (bNextAlive[Index])
		{
			NextGrid.SetAlive(Candidates[Index], true);
		}
	}
}
