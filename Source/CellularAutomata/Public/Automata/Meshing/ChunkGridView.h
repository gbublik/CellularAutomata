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
 *  масштабе чанков.
 *
 *  А вот GridToWorld() переопределить ПРИДЁТСЯ, и это не мелочь: базовая
 *  реализация (Cell * CellSize) ставила бы куб чанка центром в
 *  Чанк*РазмерЧанка, тогда как настоящий центр чанка лежит на полчанка
 *  дальше - минус полклетки, потому что GridToWorld() у клеток даёт их
 *  ЦЕНТРЫ, а не углы. Чанк C занимает клетки от C*ChunkSize до
 *  C*ChunkSize+ChunkSize-1, то есть в мире тянется от
 *  C*РазмерЧанка - CellSize/2 до C*РазмерЧанка + РазмерЧанка - CellSize/2.
 *  Без этой поправки ghost-силуэт уезжает на (РазмерЧанка - CellSize)/2 по
 *  каждой оси - при ChunkSize=16 и CellSize=100 это 750 юнитов, что и
 *  наблюдалось живьём как зазор между силуэтом и детальными клетками.
 *
 *  Тот же идиом, что и у FFilteredCellGridView (Automata/Rendering/
 *  FilteredCellGridView.h) - мутирующие методы no-op, реальных данных
 *  (IsAlive/GetAge) тут нет и не нужно: BuildFromCells() их не вызывает. */
class CELLULARAUTOMATA_API FChunkGridView : public FCellGrid
{
public:
	/** bBuildOccupancySet - построить множество занятых чанков, чтобы
	 *  IsAlive() отвечал по-настоящему. Строителю меша оно не нужно (он
	 *  проверяет принадлежность своим способом), а вот пикингу лучом
	 *  необходимо: CellSelection::PickCellAlongRay() ищет ПЕРВУЮ живую
	 *  ячейку на пути, и с заглушкой "жива всегда" он вернул бы первый же
	 *  чанк, который задел луч, включая пустые. Множество на десятки тысяч
	 *  чанков стоит микросекунды, но строить его в гост-рендере, где оно не
	 *  нужно, незачем - отсюда флаг. */
	FChunkGridView(float InChunkWorldSize, float InCellSize, TArray<FIntVector> InOccupiedChunkCoords, bool bBuildOccupancySet = false)
		: FCellGrid(InChunkWorldSize)
		, ChunkCenterOffset((InChunkWorldSize - InCellSize) * 0.5f)
		, OccupiedChunkCoords(MoveTemp(InOccupiedChunkCoords))
	{
		if (bBuildOccupancySet)
		{
			OccupiedChunkSet.Append(OccupiedChunkCoords);
		}
	}

	/** См. doc-comment класса: центр куба чанка, а не его угол. CellSize
	 *  базового класса здесь хранит РАЗМЕР ЧАНКА (так задумано), поэтому
	 *  первое слагаемое и есть Чанк*РазмерЧанка. */
	virtual FVector GridToWorld(const FIntVector& Chunk) const override
	{
		return FVector(Chunk.X, Chunk.Y, Chunk.Z) * CellSize + FVector(ChunkCenterOffset);
	}

	/** true всегда, если множество занятости не строилось (так было изначально
	 *  и так достаточно строителю меша); иначе честный ответ по нему - см.
	 *  bBuildOccupancySet в конструкторе. */
	virtual bool IsAlive(const FIntVector& Chunk) const override
	{
		return OccupiedChunkSet.IsEmpty() ? true : OccupiedChunkSet.Contains(Chunk);
	}
	virtual void SetAlive(const FIntVector& Cell, bool bAlive) override {}
	virtual void Clear() override {}
	virtual int32 Num() const override { return OccupiedChunkCoords.Num(); }
	virtual void GetAliveCells(TArray<FIntVector>& OutCells) const override { OutCells = OccupiedChunkCoords; }
	virtual uint8 GetAge(const FIntVector& Cell) const override { return 0; }
	virtual void SetAge(const FIntVector& Cell, uint8 Age) override {}

private:
	/** (РазмерЧанка - CellSize)/2 - см. GridToWorld() выше. */
	float ChunkCenterOffset;

	TArray<FIntVector> OccupiedChunkCoords;

	/** Пустое, если конструктор звали без bBuildOccupancySet - см. IsAlive(). */
	TSet<FIntVector> OccupiedChunkSet;
};
