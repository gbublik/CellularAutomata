#include "Automata/Simulation/LatticeNeighborhood.h"

TArray<FIntVector> BuildLatticeNeighborOffsets(ELatticeNeighborhood Neighborhood)
{
	TArray<FIntVector> Offsets;

	switch (Neighborhood)
	{
	case ELatticeNeighborhood::ElongatedDodecahedron12:
		// Порядок фиксирован (сначала восемь диагоналей, потом четыре дальние
		// оси) по той же причине, что и у BuildNeighborOffsets(): набор уезжает
		// в шейдерный массив фиксированной длины, и воспроизводимость сравнения
		// CPU против GPU держится на совпадении порядка.
		Offsets.Reserve(12);
		for (int32 dz = -1; dz <= 1; dz += 2)
		{
			for (int32 dy = -1; dy <= 1; dy += 2)
			{
				for (int32 dx = -1; dx <= 1; dx += 2)
				{
					Offsets.Emplace(dx, dy, dz);
				}
			}
		}
		// Только X и Y: грань к (0,0,+-2) на растянутой решётке не существует -
		// см. doc-comment значения.
		Offsets.Emplace(2, 0, 0);
		Offsets.Emplace(-2, 0, 0);
		Offsets.Emplace(0, 2, 0);
		Offsets.Emplace(0, -2, 0);
		break;

	case ELatticeNeighborhood::Shells:
	default:
		// Пусто - сигнал "бери обычный ENeighborhood", а не "соседей нет".
		break;
	}

	return Offsets;
}

FString GetLatticeNeighborhoodDisplayName(ELatticeNeighborhood Neighborhood)
{
	switch (Neighborhood)
	{
	case ELatticeNeighborhood::ElongatedDodecahedron12:
		return TEXT("Удлинённый додекаэдр (12)");
	case ELatticeNeighborhood::Shells:
	default:
		return TEXT("Оболочки");
	}
}
