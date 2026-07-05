#pragma once

#include "CoreMinimal.h"
#include "Automata/Simulation/ComputeStrategy/CellularAutomatonComputeStrategy.h"

/**
 * Заглушка на будущее: интерфейс реализован, но реального GPU compute
 * shader пока нет (требует RHI/RenderCore, каталог Shaders/, RDG dispatch -
 * отдельная задача). Пока делегирует на FCpuComputeStrategy с
 * предупреждением в лог, чтобы выбор Gpu в Details panel не был "молча
 * ничего не делает".
 */
class CELLULARAUTOMATA_API FGpuComputeStrategy : public FCellularAutomatonComputeStrategy
{
public:
	virtual void Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule) const override;
};
