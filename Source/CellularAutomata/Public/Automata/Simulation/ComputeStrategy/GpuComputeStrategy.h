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

	/** Пачка поколений за один круг через GPU - см. doc-comment на базовом
	 *  классе за общим контрактом и за двумя обязательствами реализации.
	 *  Здесь пачка включается, только когда все условия сошлись (иначе честно
	 *  возвращается 1, и вызывающий цикл просто сделает следующую итерацию):
	 *   - NumSteps > 1;
	 *   - правило не в режиме Generations (см. базовый doc-comment);
	 *   - объём AABB с гало NumSteps влезает в MaxVolumeCells - иначе пачка
	 *     УРЕЗАЕТСЯ до максимального K, который влезает, а не отбрасывается
	 *     целиком (гало обязано равняться числу шагов, см. .cpp);
	 *   - объём влезает в BatchVolumeCellLimit() - отдельный, более жёсткий
	 *     лимит: пачке нужна ещё и плоскость возрастов по байту на клетку
	 *     (против бита у плоскости живых), см. её doc-comment. */
	virtual int32 StepBatch(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule, int32 NumSteps) const override;

	/** См. базовый класс. Пачки поддерживаются для любого правила, кроме
	 *  Generations. Остальные ограничения (объём AABB против лимитов) здесь
	 *  сознательно НЕ проверяются: они зависят от текущего состояния сетки,
	 *  меняются от поколения к поколению, и StepBatch() умеет на них
	 *  реагировать сам, урезая пачку. Отвечать "не поддерживаю" из-за
	 *  разово раздувшегося объёма значило бы навсегда выключить пачку для
	 *  всего прогона. */
	virtual bool SupportsStepBatching(const FCellularAutomatonRule& Rule) const override;

	/** См. doc-comment на базовом классе - размер входного битового буфера,
	 *  посчитанный последним Step() (0, если последний Step() откатился на
	 *  CPU из-за OOM-guard - см. его тело). */
	virtual int64 GetLastComputeUploadBytes() const override { return LastInputBufferBytes; }

	/** См. базовый doc-comment. true после любого из трёх откатов в StepBatch(). */
	virtual bool DidLastStepFallBackToCpu() const override { return bLastStepFellBackToCpu; }

private:
	/** Верхняя граница объёма AABB (в клетках) выше которой Step()
	 *  откатывается на CPU - см. AAutomataOrchestrator::GpuVolumeCellLimit. */
	int64 MaxVolumeCells;

	/** Потолок объёма для пачки (StepBatch()) - ниже MaxVolumeCells ровно во
	 *  столько раз, во сколько пачке нужно больше памяти на клетку. Обычному
	 *  шагу хватает битовой плоскости живых (бит на клетку); пачке нужна ещё
	 *  плоскость возрастов - байт на клетку, отсюда деление на 8 - а в режиме
	 *  Generations сверх того плоскость угасающих состояний, тоже байт, отсюда
	 *  16. Смысл в обоих случаях один: "пачка не займёт больше памяти, чем
	 *  MaxVolumeCells уже разрешает обычному шагу". Отдельного свойства нет
	 *  специально - граница выводится из уже существующего
	 *  GpuVolumeCellLimit и едет вместе с ним. */
	int64 BatchVolumeCellLimit(bool bDecayActive) const { return MaxVolumeCells / (bDecayActive ? 16 : 8); }

	/** См. GetLastComputeUploadBytes(). mutable - Step() сам const (общий
	 *  контракт с CPU-стратегией), это чисто диагностический побочный
	 *  эффект, не часть основной логики шага. */
	mutable int64 LastInputBufferBytes = 0;

	/** См. DidLastStepFallBackToCpu(). mutable по той же причине, что и
	 *  LastInputBufferBytes рядом: диагностический побочный эффект const-шага. */
	mutable bool bLastStepFellBackToCpu = false;
};
