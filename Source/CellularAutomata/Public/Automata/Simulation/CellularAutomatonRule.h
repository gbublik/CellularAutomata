#pragma once

#include "CoreMinimal.h"
#include "Automata/Simulation/Neighborhood.h"

class FCellGrid;

/**
 * Правило клеточного автомата: клетка рождается/выживает по количеству
 * живых соседей. Три параметра, каждый независим и однозначен - никакого
 * строкового формата и парсинга: BirthCounts/SurvivalCounts - обычные
 * списки чисел (в отличие от классической Conway-нотации "B3/S23", здесь
 * не нужна отдельная запись для счётчиков >= 10, актуальных для Moore, до
 * 26 соседей). Не владеет сетками - Step() читает CurrentGrid и пишет в
 * NextGrid (double buffering), поэтому CurrentGrid никогда не мутируется
 * во время подсчёта соседей.
 */
class CELLULARAUTOMATA_API FCellularAutomatonRule
{
public:
	FCellularAutomatonRule(const TArray<int32>& BirthCounts, const TArray<int32>& SurvivalCounts, ENeighborhood InNeighborhood);

	/** Один шаг: для каждой клетки-кандидата (живая клетка CurrentGrid или
	 *  её сосед) решает, жива ли она в следующем поколении, и если да -
	 *  вызывает NextGrid.SetAlive(..., true). NextGrid должна быть пустой
	 *  на входе. */
	void Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid) const;

private:
	static TArray<FIntVector> BuildNeighborOffsets(ENeighborhood InNeighborhood);

	TArray<FIntVector> NeighborOffsets;
	TSet<int32> BirthCounts;
	TSet<int32> SurvivalCounts;
};
