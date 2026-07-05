#pragma once

#include "CoreMinimal.h"
#include "Automata/Grid/CellGrid.h"

/** Тонкая read-only обёртка над реальной FCellGrid, отдающая через
 *  GetAliveCells() заранее отфильтрованное/забакетированное подмножество
 *  клеток (например, только клетки одного возрастного бакета материалов -
 *  см. AAutomataOrchestrator::AgeMaterials), вместо полного списка живых
 *  клеток исходной сетки.
 *
 *  Существует только для того, чтобы FInstancedMeshCellGridRenderer::
 *  BeginRender()/AdvanceRenderChunk() можно было использовать без единой
 *  правки - тот код обращается к FCellGrid исключительно через
 *  GetAliveCells()/GetCellSize()/GridToWorld() (проверено), поэтому только
 *  эти методы имеют смысл для рендера; остальные существуют лишь чтобы
 *  удовлетворить абстрактный интерфейс и никогда не вызываются рендерером -
 *  мутирующие методы (SetAlive/SetAge/Clear) поэтому no-op. */
class CELLULARAUTOMATA_API FFilteredCellGridView : public FCellGrid
{
public:
	FFilteredCellGridView(const FCellGrid& InSourceGrid, TArray<FIntVector> InFilteredCells)
		: FCellGrid(InSourceGrid.GetCellSize())
		, SourceGrid(InSourceGrid)
		, FilteredCells(MoveTemp(InFilteredCells))
	{
	}

	virtual bool IsAlive(const FIntVector& Cell) const override { return SourceGrid.IsAlive(Cell); }
	virtual void SetAlive(const FIntVector& Cell, bool bAlive) override {}
	virtual void Clear() override {}
	virtual int32 Num() const override { return FilteredCells.Num(); }
	virtual void GetAliveCells(TArray<FIntVector>& OutCells) const override { OutCells = FilteredCells; }
	virtual uint8 GetAge(const FIntVector& Cell) const override { return SourceGrid.GetAge(Cell); }
	virtual void SetAge(const FIntVector& Cell, uint8 Age) override {}

	virtual FVector GridToWorld(const FIntVector& Cell) const override { return SourceGrid.GridToWorld(Cell); }

private:
	const FCellGrid& SourceGrid;
	TArray<FIntVector> FilteredCells;
};
