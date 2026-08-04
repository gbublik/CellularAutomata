#include "Automata/Simulation/CellularAutomatonRule.h"

TArray<FIntVector> FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood InNeighborhood)
{
	// Вся геометрия соседства живёт в одной маске оболочек (см.
	// GetNeighborhoodShellMask()), а здесь она только разворачивается в список
	// смещений. Поэтому новое соседство - это строка там, а не ветка тут.
	const uint32 ShellMask = GetNeighborhoodShellMask(InNeighborhood);

	// Перебирается куб 5x5x5, потому что дальние оси имеют компоненту 2.
	// Лишнего это не втянет: отсев по d^2 <= 4 отбрасывает всё остальное
	// (ближайшее за бортом - (2,1,0) с d^2 = 5). Для наборов без дальних осей
	// результат тот же, что дал бы куб 3x3x3: любое смещение с компонентой 2
	// имеет d^2 >= 4 и в оболочки 1-3 не попадает.
	//
	// Порядок обхода (dx -> dy -> dz) выбран не произвольно: он даёт для Moore
	// ровно те же 26 смещений в том же порядке, что и первая реализация этого
	// проекта. Сам по себе порядок ни на что не влияет - и CPU-, и GPU-путь
	// только суммируют по нему, - но сохранить его дешевле, чем каждый раз
	// доказывать, что можно не сохранять.
	constexpr int32 R = 2;
	constexpr int32 MaxDistSq = 4;

	TArray<FIntVector> Offsets;
	Offsets.Reserve(26);

	for (int32 dx = -R; dx <= R; ++dx)
	{
		for (int32 dy = -R; dy <= R; ++dy)
		{
			for (int32 dz = -R; dz <= R; ++dz)
			{
				const int32 DistSq = dx * dx + dy * dy + dz * dz;
				if (DistSq == 0 || DistSq > MaxDistSq)
				{
					continue;
				}
				if (((ShellMask >> DistSq) & 1u) != 0)
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

FCellularAutomatonRule::FCellularAutomatonRule(const TArray<int32>& InBirthCounts, const TArray<int32>& InSurvivalCounts, ENeighborhood InNeighborhood, int32 InStates)
	: NeighborOffsets(BuildNeighborOffsets(InNeighborhood))
	, BirthCounts(InBirthCounts)
	, SurvivalCounts(InSurvivalCounts)
	, States(InStates)
	, NeighborExtent(ComputeNeighborExtent(NeighborOffsets))
{
}

FCellularAutomatonRule::FCellularAutomatonRule(const TArray<int32>& InBirthCounts, const TArray<int32>& InSurvivalCounts, const TArray<FIntVector>& InNeighborOffsets, int32 InStates)
	: NeighborOffsets(InNeighborOffsets)
	, BirthCounts(InBirthCounts)
	, SurvivalCounts(InSurvivalCounts)
	, States(InStates)
	, NeighborExtent(ComputeNeighborExtent(NeighborOffsets))
{
	// Дальность считается по фактическим смещениям тем же ComputeNeighborExtent(),
	// что и у набора оболочек - именно поэтому произвольный список получает
	// верное гало GPU-пачки даром. Гало меньше нужного молча теряет пограничные
	// клетки: ни падения, ни строчки в логе (см. GetNeighborExtent()).
}
