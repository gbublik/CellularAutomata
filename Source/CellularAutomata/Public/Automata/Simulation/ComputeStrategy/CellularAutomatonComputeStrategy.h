#pragma once

#include "CoreMinimal.h"

class FCellGrid;
class FCellularAutomatonRule;

/**
 * Абстрактный метод расчёта одного шага клеточного автомата - позволяет
 * подключать разные реализации (CPU, GPU, ...) без изменения кода
 * оркестратора (см. AAutomataOrchestrator::CreateComputeStrategy(),
 * зеркалящий CreateGrid()'s switch-паттерн). Синхронный/блокирующий
 * контракт: читает CurrentGrid и пишет в NextGrid (double buffering),
 * не мутирует CurrentGrid; NextGrid должна быть пустой на входе.
 * Асинхронность (StepAsync()) остаётся заботой вызывающей стороны
 * (Async(ThreadPool,...)) - стратегия, которой нужен собственный
 * асинхронный бэкенд (например GPU dispatch+readback), делает это
 * внутри своей реализации, не меняя этот контракт.
 */
class CELLULARAUTOMATA_API FCellularAutomatonComputeStrategy
{
public:
	FCellularAutomatonComputeStrategy() = default;
	virtual ~FCellularAutomatonComputeStrategy() = default;

	FCellularAutomatonComputeStrategy(const FCellularAutomatonComputeStrategy&) = delete;
	FCellularAutomatonComputeStrategy& operator=(const FCellularAutomatonComputeStrategy&) = delete;

	virtual void Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule) const = 0;

	/** Посчитать ДО NumSteps поколений подряд за один вызов, отдав в NextGrid
	 *  только последнее - промежуточные поколения наружу не выдаются вовсе.
	 *  Возвращает, сколько поколений реально продвинуто: всегда >= 1 и
	 *  <= NumSteps, так что вызывающий крутит цикл
	 *  `while (Done < NumSteps) Done += StepBatch(..., NumSteps - Done)`
	 *  и не обязан ничего знать про то, умеет ли конкретная стратегия пачки.
	 *
	 *  Смысл существования - у GPU-стратегии между поколениями стоит полный
	 *  круг CPU->GPU->CPU (упаковка, заливка, блокирующий readback, распаковка
	 *  по всему объёму AABB), и при StepsPerRender > 1 он платится за каждое
	 *  поколение, хотя на экран попадает только последнее. Пачка позволяет
	 *  залить состояние один раз, прокрутить N шагов в GPU-памяти и забрать
	 *  результат один раз.
	 *
	 *  Дефолт - ровно один Step(): стратегия без собственной поддержки пачек
	 *  (FCpuComputeStrategy) не меняется вовсе, а вызывающий цикл сам сделает
	 *  оставшиеся итерации.
	 *
	 *  ДВА ОБЯЗАТЕЛЬСТВА реализации, вернувшей > 1 (иначе возвращать 1):
	 *   - она САМА заполнила возрасты клеток в NextGrid: CellAging::ComputeAges()
	 *     умеет только диффить два соседних поколения, а промежуточных здесь
	 *     не существует - вызывающий обязан пропустить ComputeAges(), когда
	 *     получил > 1 (иначе клетка, прожившая всю пачку, получила бы +1
	 *     вместо +N, а умершая и родившаяся заново внутри пачки - неверно
	 *     засчитанную непрерывность);
	 *   - правило НЕ в режиме Generations (Rule.HasDecayStates()): угасание
	 *     продвигает CellDecay::AdvanceDecayStates() между поколениями на CPU,
	 *     внутри пачки этого прохода нет. */
	virtual int32 StepBatch(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule, int32 NumSteps) const
	{
		Step(CurrentGrid, NextGrid, Rule);
		return 1;
	}

	/** Есть ли смысл звать StepBatch() с NumSteps > 1 для ЭТОГО правила -
	 *  то есть продвинет ли стратегия больше одного поколения за вызов.
	 *  Правило параметром, потому что ответ от него зависит: GPU-стратегия
	 *  умеет пачки, но не в режиме Generations (см. второе обязательство в
	 *  doc-comment'е StepBatch() выше).
	 *
	 *  Нужен вызывающему, чтобы решить не КАК звать StepBatch(), а СТОИТ ЛИ
	 *  вообще перестраивать своё поведение под пачку. В непрерывном Play
	 *  (AAutomataOrchestrator::StepAsync()) разница принципиальна: при true
	 *  один фоновый заход считает сразу StepsPerRender поколений, при false
	 *  остаётся прежний ритм "одно поколение за заход, рендерим каждое
	 *  N-ое". Через StepBatch() это не выразить - там про то, что уже
	 *  происходит, а решение надо принять ДО. Для стратегии без поддержки
	 *  пачек (дефолт) собирать N поколений в один заход было бы чистым
	 *  проигрышем: та же работа, но одним длинным блоком, с более грубым
	 *  темпом и дольше висящим bStepInProgress. */
	virtual bool SupportsStepBatching(const FCellularAutomatonRule& Rule) const { return false; }

	/** Простая оценка объёма данных, загруженных в GPU-буфер на последнем
	 *  Step() (см. AAutomataOrchestrator::FHudStats::EstimatedGpuComputeUploadMB) -
	 *  0 по умолчанию (честный ответ "нет такой загрузки", а не то же
	 *  число, посчитанное иначе) - CPU-стратегия ничего не грузит в GPU для
	 *  расчёта, так что не переопределяет. FGpuComputeStrategy переопределяет
	 *  и возвращает уже посчитанный внутри Step() размер входного битового
	 *  буфера - без лишнего сканирования сетки специально ради этой цифры. */
	virtual int64 GetLastComputeUploadBytes() const { return 0; }
};
