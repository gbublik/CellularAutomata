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

	// Формы (не метрики) определены как подмножества куба 3x3x3 и радиуса не
	// имеют - см. IsNeighborhoodRadiusSupported(). Здесь он поэтому жёстко
	// сбрасывается в 1, а не клампится молча "почти правильно": вызов с
	// другим радиусом либо не дойдёт сюда (парсер отвергнет, оркестратор
	// приведёт), либо приходит из теста и должен получить определённый ответ.
	const bool bShape = (InNeighborhood != ENeighborhood::VonNeumann && InNeighborhood != ENeighborhood::Moore);
	const int32 EffectiveR = bShape ? 1 : R;

	// Общий случай: куб со стороной 2R+1 без центра, с отсевом под конкретное
	// соседство. Порядок обхода (dx -> dy -> dz) выбран не произвольно: при
	// R == 1 ветка Moore обязана выдать ровно те же 26 офсетов в том же
	// порядке, что и прежняя реализация.
	const int32 Side = 2 * EffectiveR + 1;
	Offsets.Reserve(Side * Side * Side - 1);

	for (int32 dx = -EffectiveR; dx <= EffectiveR; ++dx)
	{
		for (int32 dy = -EffectiveR; dy <= EffectiveR; ++dy)
		{
			for (int32 dz = -EffectiveR; dz <= EffectiveR; ++dz)
			{
				if (dx == 0 && dy == 0 && dz == 0)
				{
					continue;
				}

				// Число ненулевых компонент - это и есть класс смещения:
				// 1 - грань, 2 - ребро, 3 - диагональ (оно же d^2 для R=1).
				const int32 NonZeroComponents = (dx != 0 ? 1 : 0) + (dy != 0 ? 1 : 0) + (dz != 0 ? 1 : 0);

				bool bAccept = false;
				switch (InNeighborhood)
				{
				case ENeighborhood::VonNeumann:
					bAccept = (FMath::Abs(dx) + FMath::Abs(dy) + FMath::Abs(dz)) <= EffectiveR;
					break;
				case ENeighborhood::Moore:
					// Чебышёв: весь куб целиком, отсева нет.
					bAccept = true;
					break;
				case ENeighborhood::Edges:
					bAccept = (NonZeroComponents == 2);
					break;
				case ENeighborhood::Corners:
					bAccept = (NonZeroComponents == 3);
					break;
				case ENeighborhood::FacesEdges:
					bAccept = (NonZeroComponents <= 2);
					break;
				default:
					break;
				}

				if (bAccept)
				{
					Offsets.Add(FIntVector(dx, dy, dz));
				}
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
