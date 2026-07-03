#pragma once

#include "CoreMinimal.h"
#include "Automata/Simulation/Neighborhood.h"

class FCellGrid;

/**
 * Правило клеточного автомата в нотации "B<рождение>/S<выживание>",
 * обобщённой на 3D (счётчик соседей не ограничен 8). Не владеет
 * сетками - Step() читает CurrentGrid и пишет в NextGrid (double buffering),
 * поэтому CurrentGrid никогда не мутируется во время подсчёта соседей.
 */
class CELLULARAUTOMATA_API FCellularAutomatonRule
{
public:
	FCellularAutomatonRule(const FString& RuleString, ENeighborhood InNeighborhood);

	/** Один шаг: для каждой клетки-кандидата (живая клетка CurrentGrid или
	 *  её сосед) решает, жива ли она в следующем поколении, и если да -
	 *  вызывает NextGrid.SetAlive(..., true). NextGrid должна быть пустой
	 *  на входе. */
	void Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid) const;

private:
	static TArray<FIntVector> BuildNeighborOffsets(ENeighborhood InNeighborhood);

	/** Разбирает один сегмент правила (без ведущей буквы B/S) в набор
	 *  количеств соседей. Гибридный формат: если в сегменте есть запятая -
	 *  токены через запятую, каждый может быть многозначным числом (нужно
	 *  для Moore, где счётчик может быть > 9); иначе - классическая нотация
	 *  "каждая цифра отдельное значение" (однозначна только пока все
	 *  значения < 10, как в Von Neumann). При ошибке разбора логирует
	 *  warning и возвращает пустой TSet. */
	static TSet<int32> ParseCountSegment(const FString& Segment, const TCHAR* SegmentLabel);

	TArray<FIntVector> NeighborOffsets;
	TSet<int32> BirthCounts;
	TSet<int32> SurvivalCounts;
};
