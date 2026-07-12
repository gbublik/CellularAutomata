#pragma once

#include "CoreMinimal.h"

class FCellGrid;

/** Продвижение "Generations"-угасания - отдельный проход после
 *  FCellularAutomatonComputeStrategy::Step() (и после CellAging::ComputeAges(),
 *  хотя порядок между ними не важен - разные каналы хранения), не часть ни
 *  одной из compute-стратегий. Работает только через публичный интерфейс
 *  FCellGrid (IsAlive/GetDecayingCells/SetDecayState), поэтому не завязан на
 *  то, каким способом были посчитаны живые клетки NextGrid (CPU/GPU).
 *
 *  В отличие от CellAging::ComputeAges() - НЕ параллелится (см. doc-comment
 *  AdvanceDecayStates() ниже за причиной). */
namespace CellDecay
{
	/** Точная схема переходов (см. AAutomataOrchestrator::States за общим
	 *  описанием Generations):
	 *   - MaxStates <= 2 или OldGrid == nullptr - функция ничего не делает
	 *     (бинарный автомат либо первое поколение без OldGrid).
	 *   - Клетка была жива в OldGrid, но не выжила (не жива в NextGrid) -
	 *     начинает угасание с состояния 2.
	 *   - Клетка уже угасала в OldGrid (состояние 2..MaxStates-1) - состояние
	 *     увеличивается на 1; если новое значение достигло MaxStates -
	 *     клетка окончательно умирает (состояние 0, запись не делается -
	 *     как и у Ages, отсутствие записи означает 0).
	 *   - Клетка мертва/только что родилась (состояние 1, через IsAlive) -
	 *     эта функция её не трогает.
	 *
	 *  Вызывается сразу после CellAging::ComputeAges(OldGrid, NextGrid) в
	 *  StepAsync() и в цикле NumSteps в Next() - каждое реально посчитанное
	 *  поколение, не только отрендеренное.
	 *
	 *  НЕ ParallelFor, в отличие от CellAging: SetAge() безопасно параллелить,
	 *  потому что клетка уже жива в NewGrid (чанк уже создан сequential-
	 *  записью Step()) - у SetDecayState() этой гарантии нет: NextGrid пуста
	 *  с нуля каждое поколение, только что умершая/угасающая клетка нередко
	 *  требует ленивого создания чанка, а TMap::Add() не потокобезопасен даже
	 *  для непересекающихся ключей. Оба цикла ниже - простые последовательные
	 *  for, как WriteBack-фаза в FCpuComputeStrategy::Step(). */
	CELLULARAUTOMATA_API void AdvanceDecayStates(const FCellGrid* OldGrid, FCellGrid& NextGrid, int32 MaxStates);
}
