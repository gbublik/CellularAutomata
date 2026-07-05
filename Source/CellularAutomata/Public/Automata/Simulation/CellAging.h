#pragma once

#include "CoreMinimal.h"

class FCellGrid;

/** Вычисление возраста клеток - отдельный проход после
 *  FCellularAutomatonComputeStrategy::Step(), не часть ни одной из
 *  compute-стратегий (CPU/GPU). Работает только через публичный интерфейс
 *  FCellGrid (IsAlive/GetAge/SetAge), поэтому не завязан на то, каким
 *  способом были посчитаны живые клетки NewGrid. */
namespace CellAging
{
	/** Для каждой живой клетки в NewGrid: если она была жива в OldGrid -
	 *  возраст = OldGrid-возраст + 1 (насыщается на 255); иначе (родилась
	 *  только что) - возраст 0. OldGrid может быть nullptr (например,
	 *  только что сгенерированная случайная сетка, для которой предыдущего
	 *  поколения не существует) - тогда все живые клетки NewGrid получают
	 *  возраст 0. */
	CELLULARAUTOMATA_API void ComputeAges(const FCellGrid* OldGrid, FCellGrid& NewGrid);
}
