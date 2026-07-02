#pragma once

#include "CoreMinimal.h"
#include "Containers/BitArray.h"
#include "Automata/Grid/CellGrid.h"

/** Хранение чанками фиксированного размера (ChunkSize^3 клеток на чанк),
 *  каждый чанк - плотный битовый массив (1 бит = 1 клетка). Чанки создаются
 *  лениво при первой живой клетке в них и удаляются, когда становятся
 *  полностью пустыми - так плотное хранилище не резервирует память под
 *  всю сетку целиком, а масштабируется только там, где реально есть
 *  живые клетки. Гранулярность чанка также задел на будущее: чанк -
 *  естественная единица параллельной работы для CPU/GPU диспетчеризации
 *  (не реализовано сейчас). */
class CELLULARAUTOMATA_API FDenseCellGrid : public FCellGrid
{
public:
	static constexpr int32 DefaultChunkSize = 16;

	explicit FDenseCellGrid(float InCellSize, int32 InChunkSize = DefaultChunkSize);
	virtual ~FDenseCellGrid() override = default;

	virtual bool IsAlive(const FIntVector& Cell) const override;
	virtual void SetAlive(const FIntVector& Cell, bool bAlive) override;
	virtual void Clear() override;
	virtual int32 Num() const override;
	virtual void GetAliveCells(TArray<FIntVector>& OutCells) const override;

private:
	/** Плотный чанк ChunkSize^3 клеток. AliveCount кэширует число живых
	 *  бит, чтобы SetAlive(false) мог дёшево (O(1)) определить, опустел
	 *  ли чанк, не сканируя битовый массив целиком. */
	struct FChunk
	{
		explicit FChunk(int32 InCellsPerChunk)
		{
			Bits.Init(false, InCellsPerChunk);
		}

		TBitArray<> Bits;
		int32 AliveCount = 0;
	};

	FIntVector CellToChunkCoord(const FIntVector& Cell) const;
	int32 CellToLocalIndex(const FIntVector& Cell) const;

	int32 ChunkSize;
	int32 CellsPerChunk; // ChunkSize^3, кэшировано
	TMap<FIntVector, FChunk> Chunks;
	int32 AliveCellCount = 0;
};
