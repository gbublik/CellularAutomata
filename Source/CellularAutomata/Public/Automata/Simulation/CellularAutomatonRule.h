#pragma once

#include "CoreMinimal.h"
#include "Automata/Simulation/Neighborhood.h"

/**
 * Правило клеточного автомата: клетка рождается/выживает по количеству
 * живых соседей. Три параметра, каждый независим и однозначен - никакого
 * строкового формата и парсинга: BirthCounts/SurvivalCounts - обычные
 * списки чисел (в отличие от классической Conway-нотации "B3/S23", здесь
 * не нужна отдельная запись для счётчиков >= 10, актуальных для Moore, до
 * 26 соседей). Чистый держатель параметров - сам расчёт шага делегирован
 * сменной FCellularAutomatonComputeStrategy (см. Automata/Simulation/
 * ComputeStrategy/), чтобы CPU/GPU/другие реализации могли переиспользовать
 * одни и те же параметры правила без дублирования.
 */
class CELLULARAUTOMATA_API FCellularAutomatonRule
{
public:
	FCellularAutomatonRule(const TArray<int32>& BirthCounts, const TArray<int32>& SurvivalCounts, ENeighborhood InNeighborhood);

	const TArray<FIntVector>& GetNeighborOffsets() const { return NeighborOffsets; }
	const TSet<int32>& GetBirthCounts() const { return BirthCounts; }
	const TSet<int32>& GetSurvivalCounts() const { return SurvivalCounts; }

private:
	static TArray<FIntVector> BuildNeighborOffsets(ENeighborhood InNeighborhood);

	TArray<FIntVector> NeighborOffsets;
	TSet<int32> BirthCounts;
	TSet<int32> SurvivalCounts;
};
