#pragma once

#include "CoreMinimal.h"
#include "Automata/Grid/CellGrid.h"

/** Хранение на TSet<FIntVector> - подходит для больших почти пустых сеток
 *  (текущая scatter-генерация), не резервирует память под GridSize^3. */
class CELLULARAUTOMATA_API FSparseCellGrid : public FCellGrid
{
public:
	explicit FSparseCellGrid(float InCellSize);
	virtual ~FSparseCellGrid() override = default;

	virtual bool IsAlive(const FIntVector& Cell) const override;
	virtual void SetAlive(const FIntVector& Cell, bool bAlive) override;
	virtual void Clear() override;
	virtual int32 Num() const override;
	virtual void GetAliveCells(TArray<FIntVector>& OutCells) const override;

private:
	TSet<FIntVector> AliveCells;
};
