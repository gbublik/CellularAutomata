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
};
