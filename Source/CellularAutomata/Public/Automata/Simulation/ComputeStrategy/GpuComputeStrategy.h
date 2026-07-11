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

	/** См. doc-comment на базовом классе - размер входного битового буфера,
	 *  посчитанный последним Step() (0, если последний Step() откатился на
	 *  CPU из-за OOM-guard - см. его тело). */
	virtual int64 GetLastComputeUploadBytes() const override { return LastInputBufferBytes; }

private:
	/** Верхняя граница объёма AABB (в клетках) выше которой Step()
	 *  откатывается на CPU - см. AAutomataOrchestrator::GpuVolumeCellLimit. */
	int64 MaxVolumeCells;

	/** См. GetLastComputeUploadBytes(). mutable - Step() сам const (общий
	 *  контракт с CPU-стратегией), это чисто диагностический побочный
	 *  эффект, не часть основной логики шага. */
	mutable int64 LastInputBufferBytes = 0;
};
