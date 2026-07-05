#include "Automata/Simulation/ComputeStrategy/GpuComputeStrategy.h"
#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"

void FGpuComputeStrategy::Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule) const
{
	UE_LOG(LogTemp, Warning, TEXT("FGpuComputeStrategy::Step: GPU-расчёт ещё не реализован - выполняется fallback на CPU"));
	FCpuComputeStrategy CpuFallback;
	CpuFallback.Step(CurrentGrid, NextGrid, Rule);
}
