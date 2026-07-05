#pragma once

#include "CoreMinimal.h"

/**
 * Абстрактное хранилище клеток клеточного автомата, адресуемых целыми
 * координатами сетки (FIntVector). Не знает, как клетки отрисовываются.
 */
class CELLULARAUTOMATA_API FCellGrid
{
public:
	explicit FCellGrid(float InCellSize)
		: CellSize(InCellSize)
	{
	}

	virtual ~FCellGrid() = default;

	FCellGrid(const FCellGrid&) = delete;
	FCellGrid& operator=(const FCellGrid&) = delete;

	virtual bool IsAlive(const FIntVector& Cell) const = 0;
	virtual void SetAlive(const FIntVector& Cell, bool bAlive) = 0;
	virtual void Clear() = 0;
	virtual int32 Num() const = 0;

	/** Возраст клетки - сколько поколений подряд она прожила (0 у только
	 *  что родившейся). Насыщающийся (не переполняется) - для клеток,
	 *  которых сейчас нет в сетке, возвращает 0. Используется отображением
	 *  клеток на материалы по возрасту (см. AAutomataOrchestrator::AgeMaterials). */
	virtual uint8 GetAge(const FIntVector& Cell) const = 0;
	virtual void SetAge(const FIntVector& Cell, uint8 Age) = 0;

	/** Заполняет OutCells координатами всех живых клеток (out-параметр,
	 *  чтобы не копировать внутреннее хранилище на каждый вызов). */
	virtual void GetAliveCells(TArray<FIntVector>& OutCells) const = 0;

	float GetCellSize() const { return CellSize; }

	/** Преобразование координат клетки в мировые координаты. Виртуальный,
	 *  т.к. зависит не только от CellSize, но и от топологии сетки
	 *  (кубическая, гексагональная и т.д.) - реализация по умолчанию
	 *  предполагает кубическую решётку. */
	virtual FVector GridToWorld(const FIntVector& Cell) const
	{
		return FVector(Cell.X, Cell.Y, Cell.Z) * CellSize;
	}

protected:
	float CellSize;
};
