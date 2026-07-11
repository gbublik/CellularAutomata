#pragma once

#include "CoreMinimal.h"
#include "Automata/Grid/CellGrid.h"

/** Тонкая read-only обёртка над списком координат ЧАНКОВ (не клеток) -
 *  позволяет скормить их в CellMeshBuilder::BuildFromCells() без единой
 *  правки в самом builder'е (см. AAutomataOrchestrator::RefreshGhostShape()
 *  и план "Ghost Shape"). BuildFromCells() делает face-culling чисто по
 *  membership в TSet, построенном из переданного списка координат, и
 *  использует Grid только для GetCellSize()/GridToWorld() - если
 *  подставить сюда ChunkWorldSize вместо обычного CellSize, а "клетками"
 *  считать координаты чанков, получается ровно та же геометрия, но в
 *  масштабе чанков. GridToWorld() даже переопределять не нужно - базовая
 *  реализация FCellGrid::GridToWorld() (Cell * CellSize) уже даёт нужный
 *  результат, раз CellSize здесь - это размер чанка, а не клетки.
 *
 *  Тот же идиом, что и у FFilteredCellGridView (Automata/Rendering/
 *  FilteredCellGridView.h) - мутирующие методы no-op, реальных данных
 *  (IsAlive/GetAge) тут нет и не нужно: BuildFromCells() их не вызывает. */
class CELLULARAUTOMATA_API FChunkGridView : public FCellGrid
{
public:
	FChunkGridView(float InChunkWorldSize, TArray<FIntVector> InOccupiedChunkCoords)
		: FCellGrid(InChunkWorldSize)
		, OccupiedChunkCoords(MoveTemp(InOccupiedChunkCoords))
	{
	}

	virtual bool IsAlive(const FIntVector& Cell) const override { return true; }
	virtual void SetAlive(const FIntVector& Cell, bool bAlive) override {}
	virtual void Clear() override {}
	virtual int32 Num() const override { return OccupiedChunkCoords.Num(); }
	virtual void GetAliveCells(TArray<FIntVector>& OutCells) const override { OutCells = OccupiedChunkCoords; }
	virtual uint8 GetAge(const FIntVector& Cell) const override { return 0; }
	virtual void SetAge(const FIntVector& Cell, uint8 Age) override {}

private:
	TArray<FIntVector> OccupiedChunkCoords;
};
