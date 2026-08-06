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

	/** Возвращает 0, когда шаг не влезает (см. CanStep()) - NextGrid при этом
	 *  НЕ заполнена, и вызывающий обязан не подставлять её вместо сетки. Иначе
	 *  как обычно: один шаг, возврат 1. */
	virtual int32 StepBatch(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule, int32 NumSteps) const override;

	/** Хватит ли на шаг индексов и памяти. Проверять ОБЯЗАТЕЛЬНО до Step():
	 *  без этого на больших сетках был не отказ, а падение редактора.
	 *
	 *  Дедуп кандидатов раскладывает живые*(соседей+1) координат в плоский
	 *  массив, и у этого два независимых потолка. Первый - индексный: TArray
	 *  индексируется int32, так что элементов не может быть больше MAX_int32, и
	 *  при 26 соседях это ровно 79 536 431 живая клетка. Раньше сумма молча
	 *  переполнялась в минус и SetNumUninitialized() падал внутри Array.h -
	 *  наблюдалось живьём на ~85 млн клеток, когда GPU впервые отказал
	 *  по-настоящему и работа ушла сюда. Второй потолок - физическая память:
	 *  FIntVector это 12 байт, и те же 2.29 млрд кандидатов запросили бы 27 ГБ.
	 *
	 *  Расширением до int64 первое не лечится, потому что упирается второе:
	 *  на этих размерах алгоритм не медленный, а неисполнимый.
	 *
	 *  OutReason - готовая для лога причина отказа. */
	static bool CanStep(int32 AliveCount, int32 NeighborOffsetCount, FString& OutReason);
};
