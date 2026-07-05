#pragma once

#include "CoreMinimal.h"
#include "Automata/Simulation/ComputeStrategy/CellularAutomatonComputeStrategy.h"

/**
 * CPU-реализация шага: bucket-partitioned параллельный дедуп кандидатов +
 * параллельный подсчёт соседей (см. .cpp для полного описания 4 фаз).
 * Перенесена без изменений из прежнего FCellularAutomatonRule::Step().
 */
class CELLULARAUTOMATA_API FCpuComputeStrategy : public FCellularAutomatonComputeStrategy
{
public:
	virtual void Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule) const override;
};
