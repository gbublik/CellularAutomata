#pragma once

#include "CoreMinimal.h"
#include "Automata/Grid/CellGrid.h"

/** Тонкая read-only обёртка над реальной FCellGrid, отдающая через
 *  GetAliveCells() заранее отфильтрованное/забакетированное подмножество
 *  клеток (например, только клетки одного возрастного бакета материалов -
 *  см. AAutomataOrchestrator::AgeMaterials, или только клетки внутри
 *  ARenderCullVolume - см. AAutomataOrchestrator::SelectCellsInScreenRect()/
 *  SelectCellUnderCursor()), вместо полного списка живых клеток исходной
 *  сетки.
 *
 *  Изначально существовала только для того, чтобы FInstancedMeshCellGridRenderer::
 *  BeginRender()/AdvanceRenderChunk() можно было использовать без единой
 *  правки - тот код обращается к FCellGrid исключительно через
 *  GetAliveCells()/GetCellSize()/GridToWorld(), IsAlive() ему не нужен вовсе.
 *  IsAlive() поэтому СОГЛАСОВАН с отфильтрованным подмножеством (клетка
 *  считается живой С ТОЧКИ ЗРЕНИЯ ЭТОГО ВЬЮ, только если она есть в
 *  FilteredCells), а не форвардится на исходную сетку целиком - иначе
 *  Automata/Selection/CellSelection::PickCellAlongRay() (единственный
 *  потребитель IsAlive() через этот класс - DDA-обход луча) видел бы клетки
 *  снаружи отфильтрованной области как живые, хотя вью специально построен,
 *  чтобы их скрыть (см. doc-comment SelectCellUnderCursor()). Множество для
 *  поиска строится ЛЕНИВО, при первом вызове IsAlive() - рендер-путь его
 *  вообще не вызывает, так что цена хэширования не ложится на самый горячий
 *  путь (BuildAgeBuckets()+AddInstances каждое поколение), только на
 *  разовые действия выделения. Мутирующие методы (SetAlive/SetAge/Clear)
 *  по-прежнему no-op - вью read-only. */
class CELLULARAUTOMATA_API FFilteredCellGridView : public FCellGrid
{
public:
	FFilteredCellGridView(const FCellGrid& InSourceGrid, TArray<FIntVector> InFilteredCells)
		: FCellGrid(InSourceGrid.GetCellSize())
		, SourceGrid(InSourceGrid)
		, FilteredCells(MoveTemp(InFilteredCells))
	{
	}

	virtual bool IsAlive(const FIntVector& Cell) const override
	{
		if (!bFilteredCellSetBuilt)
		{
			FilteredCellSet.Append(FilteredCells);
			bFilteredCellSetBuilt = true;
		}
		return FilteredCellSet.Contains(Cell);
	}
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
	mutable TSet<FIntVector> FilteredCellSet;
	mutable bool bFilteredCellSetBuilt = false;
};
