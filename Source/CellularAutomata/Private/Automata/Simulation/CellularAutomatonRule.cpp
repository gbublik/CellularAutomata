#include "Automata/Simulation/CellularAutomatonRule.h"

TArray<FIntVector> FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood InNeighborhood, int32 Radius)
{
	const int32 R = FMath::Max(1, Radius);
	TArray<FIntVector> Offsets;

	// Радиус 1 у фон Неймана отдаётся дословно прежним списком - в прежнем
	// порядке. Общий цикл ниже дал бы тот же НАБОР, но в другом порядке, а
	// порядок здесь ничего не стоит сохранить (см. doc-comment в заголовке).
	if (InNeighborhood == ENeighborhood::VonNeumann && R == 1)
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

	// Общий случай: куб со стороной 2R+1 без центра, у фон Неймана - ещё и
	// с отсевом по Манхэттену. Порядок обхода (dx -> dy -> dz) выбран не
	// произвольно: при R == 1 ветка Moore обязана выдать ровно те же 26
	// офсетов в том же порядке, что и прежняя реализация.
	const bool bMoore = (InNeighborhood == ENeighborhood::Moore);
	const int32 Side = 2 * R + 1;
	Offsets.Reserve(Side * Side * Side - 1);

	for (int32 dx = -R; dx <= R; ++dx)
	{
		for (int32 dy = -R; dy <= R; ++dy)
		{
			for (int32 dz = -R; dz <= R; ++dz)
			{
				if (dx == 0 && dy == 0 && dz == 0)
				{
					continue;
				}
				if (!bMoore && (FMath::Abs(dx) + FMath::Abs(dy) + FMath::Abs(dz)) > R)
				{
					continue;
				}
				Offsets.Add(FIntVector(dx, dy, dz));
			}
		}
	}
	return Offsets;
}

FCellularAutomatonRule::FCellularAutomatonRule(const TArray<int32>& InBirthCounts, const TArray<int32>& InSurvivalCounts, ENeighborhood InNeighborhood, int32 InStates, int32 InRadius)
	: NeighborOffsets(BuildNeighborOffsets(InNeighborhood, InRadius))
	, BirthCounts(InBirthCounts)
	, SurvivalCounts(InSurvivalCounts)
	, States(InStates)
	, NeighborRadius(FMath::Max(1, InRadius))
{
}
