#pragma once

#include "CoreMinimal.h"
#include "Automata/Grid/CellGrid.h"

/** Тонкая read-only обёртка над реальной FCellGrid, отдающая через
 *  GetAliveCells() заранее отфильтрованное подмножество клеток (только клетки
 *  внутри ARenderCullVolume - см. AAutomataOrchestrator::SelectCellsInScreenRect()/
 *  SelectCellUnderCursor()), вместо полного списка живых клеток исходной
 *  сетки.
 *
 *  ИСТОРИЯ: изначально существовала ради рендера - чтобы
 *  FInstancedMeshCellGridRenderer::BeginRender()/AdvanceRenderChunk() можно
 *  было скармливать один возрастной бакет материалов без единой правки в них
 *  самих. Рендер-путь этот класс больше не использует: цвет клетки уехал в
 *  per-instance custom data, бакеты исчезли, а рендерер теперь принимает явный
 *  список FCellRenderInstance. Остался единственный потребитель - DDA-обход
 *  луча при выделении (ниже), и именно под него класс и стоит читать.
 *  IsAlive() СОГЛАСОВАН с отфильтрованным подмножеством (клетка
 *  считается живой С ТОЧКИ ЗРЕНИЯ ЭТОГО ВЬЮ, только если она есть в
 *  FilteredCells), а не форвардится на исходную сетку целиком - иначе
 *  Automata/Selection/CellSelection::PickCellAlongRay() (единственный
 *  потребитель IsAlive() через этот класс - DDA-обход луча) видел бы клетки
 *  снаружи отфильтрованной области как живые, хотя вью специально построен,
 *  чтобы их скрыть (см. doc-comment SelectCellUnderCursor()). Множество для
 *  поиска строится ЛЕНИВО, при первом вызове IsAlive() - так цена хэширования
 *  платится только по факту, на разовых действиях выделения.
 *  Мутирующие методы (SetAlive/SetAge/Clear) - no-op, вью read-only. */
class CELLULARAUTOMATA_API FFilteredCellGridView : public FCellGrid
{
public:
	/** Решётка берётся у источника ЦЕЛИКОМ, а не по одному числу CellSize:
	 *  иначе вью с неравным шагом по осям молча выпрямился бы в кубический и
	 *  DDA-пик через него бил бы мимо. */
	FFilteredCellGridView(const FCellGrid& InSourceGrid, TArray<FIntVector> InFilteredCells)
		: FCellGrid(InSourceGrid.GetLattice())
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

	// GridToWorld() СПЕЦИАЛЬНО не переопределён: вью хранит копию решётки
	// источника, поэтому базовая реализация даёт тот же ответ. Прежний
	// override просто передавал вызов дальше и стоил ВТОРОГО виртуального
	// вызова на клетку - в DDA-обходе луча это была диспетчеризация ради
	// диспетчеризации. Заодно это то, что делает законным приём "взять
	// GetLattice() один раз перед горячим циклом" (см. FCellGrid).

private:
	const FCellGrid& SourceGrid;
	TArray<FIntVector> FilteredCells;
	mutable TSet<FIntVector> FilteredCellSet;
	mutable bool bFilteredCellSetBuilt = false;
};
