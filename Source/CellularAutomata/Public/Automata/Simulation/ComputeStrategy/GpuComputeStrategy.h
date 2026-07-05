#pragma once

#include "CoreMinimal.h"
#include "Automata/Simulation/ComputeStrategy/CellularAutomatonComputeStrategy.h"

/**
 * RDG compute-shader реализация Step() (см. GpuComputeStrategy.cpp). Живых
 * клеток AABB (+halo) упаковывается в битовый буфер и считается на GPU -
 * если объём AABB превышает InMaxVolumeCells (защита от OOM на редкий
 * случай далеко разлетевшихся живых клеток), откатывается на
 * FCpuComputeStrategy с предупреждением в лог, чтобы выбор Gpu в Details
 * panel никогда не был "молча ничего не делает".
 */
class CELLULARAUTOMATA_API FGpuComputeStrategy : public FCellularAutomatonComputeStrategy
{
public:
	explicit FGpuComputeStrategy(int64 InMaxVolumeCells)
		: MaxVolumeCells(InMaxVolumeCells)
	{
	}

	virtual void Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule) const override;

private:
	/** Верхняя граница объёма AABB (в клетках) выше которой Step()
	 *  откатывается на CPU - см. AAutomataOrchestrator::GpuVolumeCellLimit. */
	int64 MaxVolumeCells;
};
