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

	// Формы (не метрики) определены как наборы ОБОЛОЧЕК, различаемых квадратом
	// длины смещения: 1 - грани, 2 - рёбра, 3 - диагонали, 4 - дальние оси
	// (см. ENeighborhood). Радиуса у них нет - см.
	// IsNeighborhoodRadiusSupported(); запрошенный игнорируется, а не
	// клампится молча "почти правильно".
	//
	// Маска нулевая ровно у метрик - это и служит признаком "не форма",
	// отдельного флага не нужно.
	uint32 ShellMask = 0;
	switch (InNeighborhood)
	{
	case ENeighborhood::Edges:          ShellMask = (1u << 2); break;
	case ENeighborhood::Corners:        ShellMask = (1u << 3); break;
	case ENeighborhood::FacesEdges:     ShellMask = (1u << 1) | (1u << 2); break;
	case ENeighborhood::FarAxes:        ShellMask = (1u << 4); break;
	case ENeighborhood::EdgesFarAxes:   ShellMask = (1u << 2) | (1u << 4); break;
	case ENeighborhood::CornersFarAxes: ShellMask = (1u << 3) | (1u << 4); break;
	default: break;
	}

	// Формы всегда перебираются по кубу 5x5x5: дальние оси имеют компоненту 2,
	// а лишнего это не втянет - отсев по d^2 <= 4 отбрасывает всё остальное
	// (ближайшее за бортом - (2,1,0) с d^2 = 5). Для форм без дальних осей
	// результат тот же, что дал бы куб 3x3x3: смещения с компонентой 2 имеют
	// d^2 >= 4 и в оболочки 1-3 не попадают.
	const int32 EffectiveR = (ShellMask != 0) ? 2 : R;

	// Порядок обхода (dx -> dy -> dz) выбран не произвольно: при R == 1 ветка
	// Moore обязана выдать ровно те же 26 офсетов в том же порядке, что и
	// прежняя реализация.
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

				bool bAccept = false;
				if (ShellMask != 0)
				{
					const int32 DistSq = dx * dx + dy * dy + dz * dz;
					bAccept = (DistSq <= 4) && ((ShellMask >> DistSq) & 1u) != 0;
				}
				else if (InNeighborhood == ENeighborhood::VonNeumann)
				{
					bAccept = (FMath::Abs(dx) + FMath::Abs(dy) + FMath::Abs(dz)) <= EffectiveR;
				}
				else
				{
					// Moore, метрика Чебышёва: весь куб целиком, отсева нет.
					bAccept = true;
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

int32 FCellularAutomatonRule::ComputeNeighborExtent(const TArray<FIntVector>& Offsets)
{
	int32 Extent = 1;
	for (const FIntVector& Offset : Offsets)
	{
		Extent = FMath::Max(Extent, FMath::Max3(FMath::Abs(Offset.X), FMath::Abs(Offset.Y), FMath::Abs(Offset.Z)));
	}
	return Extent;
}

FCellularAutomatonRule::FCellularAutomatonRule(const TArray<int32>& InBirthCounts, const TArray<int32>& InSurvivalCounts, ENeighborhood InNeighborhood, int32 InStates, int32 InRadius)
	: NeighborOffsets(BuildNeighborOffsets(InNeighborhood, InRadius))
	, BirthCounts(InBirthCounts)
	, SurvivalCounts(InSurvivalCounts)
	, States(InStates)
	, NeighborRadius(FMath::Max(1, InRadius))
	, NeighborExtent(ComputeNeighborExtent(NeighborOffsets))
{
}
