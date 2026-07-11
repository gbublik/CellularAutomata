#include "Automata/Grid/DenseCellGrid.h"

namespace
{
	/** Деление с округлением к минус бесконечности (floor division).
	 *  НЕ совпадает с FMath::DivideAndRoundDown для целых чисел - та
	 *  функция вопреки названию просто делает Dividend / Divisor
	 *  (усечение к нулю), поэтому DivideAndRoundDown(-1, 16) == 0, а не
	 *  -1. Здесь это принципиально важно: генерация клеток уводит
	 *  координаты в отрицательную область. Divisor предполагается > 0. */
	FORCEINLINE int32 FloorDiv(int32 Dividend, int32 Divisor)
	{
		const int32 Quotient = Dividend / Divisor;
		const int32 Remainder = Dividend % Divisor;
		return (Remainder != 0 && Remainder < 0) ? Quotient - 1 : Quotient;
	}

	/** Остаток по модулю, приведённый к диапазону [0, Divisor) - в
	 *  отличие от оператора %, который для отрицательного Dividend в
	 *  C++ возвращает отрицательный (или нулевой) результат. Divisor
	 *  предполагается > 0. */
	FORCEINLINE int32 PositiveMod(int32 Dividend, int32 Divisor)
	{
		const int32 Remainder = Dividend % Divisor;
		return Remainder < 0 ? Remainder + Divisor : Remainder;
	}
}

FDenseCellGrid::FDenseCellGrid(float InCellSize, int32 InChunkSize)
	: FCellGrid(InCellSize)
	, ChunkSize(FMath::Max(1, InChunkSize))
	, CellsPerChunk(ChunkSize * ChunkSize * ChunkSize)
{
}

FIntVector FDenseCellGrid::CellToChunkCoord(const FIntVector& Cell) const
{
	return FIntVector(
		FloorDiv(Cell.X, ChunkSize),
		FloorDiv(Cell.Y, ChunkSize),
		FloorDiv(Cell.Z, ChunkSize));
}

int32 FDenseCellGrid::CellToLocalIndex(const FIntVector& Cell) const
{
	const int32 LocalX = PositiveMod(Cell.X, ChunkSize);
	const int32 LocalY = PositiveMod(Cell.Y, ChunkSize);
	const int32 LocalZ = PositiveMod(Cell.Z, ChunkSize);
	return (LocalZ * ChunkSize + LocalY) * ChunkSize + LocalX;
}

bool FDenseCellGrid::IsAlive(const FIntVector& Cell) const
{
	const FChunk* Chunk = Chunks.Find(CellToChunkCoord(Cell));
	return Chunk && Chunk->Bits[CellToLocalIndex(Cell)];
}

void FDenseCellGrid::SetAlive(const FIntVector& Cell, bool bAlive)
{
	const FIntVector ChunkCoord = CellToChunkCoord(Cell);
	const int32 LocalIndex = CellToLocalIndex(Cell);

	if (bAlive)
	{
		FChunk* Chunk = Chunks.Find(ChunkCoord);
		if (!Chunk)
		{
			// Ленивое создание чанка - только когда в нём появляется
			// первая живая клетка.
			Chunk = &Chunks.Add(ChunkCoord, FChunk(CellsPerChunk));
		}

		if (!Chunk->Bits[LocalIndex])
		{
			Chunk->Bits[LocalIndex] = true;
			++Chunk->AliveCount;
			++AliveCellCount;
		}
	}
	else
	{
		FChunk* Chunk = Chunks.Find(ChunkCoord);
		if (!Chunk)
		{
			return;
		}

		if (Chunk->Bits[LocalIndex])
		{
			Chunk->Bits[LocalIndex] = false;
			--Chunk->AliveCount;
			--AliveCellCount;

			if (Chunk->AliveCount == 0)
			{
				// Чанк опустел - высвобождаем память под него полностью.
				Chunks.Remove(ChunkCoord);
			}
		}
	}
}

uint8 FDenseCellGrid::GetAge(const FIntVector& Cell) const
{
	const FChunk* Chunk = Chunks.Find(CellToChunkCoord(Cell));
	return Chunk ? Chunk->Ages[CellToLocalIndex(Cell)] : 0;
}

void FDenseCellGrid::SetAge(const FIntVector& Cell, uint8 Age)
{
	// Возраст осмысленно задавать только у живой клетки - чанк для мёртвой
	// координаты может вообще не существовать (лениво создаётся только
	// SetAlive(true)), поэтому здесь намеренно не создаём чанк сами.
	FChunk* Chunk = Chunks.Find(CellToChunkCoord(Cell));
	if (Chunk)
	{
		Chunk->Ages[CellToLocalIndex(Cell)] = Age;
	}
}

void FDenseCellGrid::Clear()
{
	Chunks.Empty();
	AliveCellCount = 0;
}

int32 FDenseCellGrid::Num() const
{
	return AliveCellCount;
}

void FDenseCellGrid::GetAliveCells(TArray<FIntVector>& OutCells) const
{
	OutCells.Reset(AliveCellCount);

	for (const TPair<FIntVector, FChunk>& ChunkPair : Chunks)
	{
		const FIntVector ChunkOrigin = ChunkPair.Key * ChunkSize;
		const FChunk& Chunk = ChunkPair.Value;

		for (TConstSetBitIterator<> It(Chunk.Bits); It; ++It)
		{
			const int32 LocalIndex = It.GetIndex();
			const int32 LocalX = LocalIndex % ChunkSize;
			const int32 LocalY = (LocalIndex / ChunkSize) % ChunkSize;
			const int32 LocalZ = LocalIndex / (ChunkSize * ChunkSize);

			OutCells.Add(ChunkOrigin + FIntVector(LocalX, LocalY, LocalZ));
		}
	}
}

void FDenseCellGrid::GetAliveCellsInBounds(const FBox& WorldBounds, TArray<FIntVector>& OutCells) const
{
	OutCells.Reset();

	// WorldBounds -> границы в клеточном пространстве. Min - floor, Max -
	// ceil, чтобы ни одна частично захваченная клетка не потерялась на
	// краю (сама по-клеточная проверка ниже, для граничных чанков, уже
	// точная - здесь достаточно консервативной, чуть более широкой рамки).
	const FIntVector MinCell(
		FMath::FloorToInt(WorldBounds.Min.X / CellSize),
		FMath::FloorToInt(WorldBounds.Min.Y / CellSize),
		FMath::FloorToInt(WorldBounds.Min.Z / CellSize));
	const FIntVector MaxCell(
		FMath::CeilToInt(WorldBounds.Max.X / CellSize),
		FMath::CeilToInt(WorldBounds.Max.Y / CellSize),
		FMath::CeilToInt(WorldBounds.Max.Z / CellSize));

	for (const TPair<FIntVector, FChunk>& ChunkPair : Chunks)
	{
		const FIntVector ChunkOrigin = ChunkPair.Key * ChunkSize;
		const FIntVector ChunkMax = ChunkOrigin + FIntVector(ChunkSize - 1, ChunkSize - 1, ChunkSize - 1);

		// Чанк вообще не пересекает запрошенные границы - пропускаем
		// целиком, ни один бит не читаем.
		const bool bOverlaps =
			ChunkOrigin.X <= MaxCell.X && ChunkMax.X >= MinCell.X &&
			ChunkOrigin.Y <= MaxCell.Y && ChunkMax.Y >= MinCell.Y &&
			ChunkOrigin.Z <= MaxCell.Z && ChunkMax.Z >= MinCell.Z;
		if (!bOverlaps)
		{
			continue;
		}

		// Чанк целиком внутри границ - каждая живая клетка в нём гарантированно
		// проходит фильтр, поклеточная проверка не нужна (как в GetAliveCells()).
		const bool bFullyContained =
			ChunkOrigin.X >= MinCell.X && ChunkMax.X <= MaxCell.X &&
			ChunkOrigin.Y >= MinCell.Y && ChunkMax.Y <= MaxCell.Y &&
			ChunkOrigin.Z >= MinCell.Z && ChunkMax.Z <= MaxCell.Z;

		const FChunk& Chunk = ChunkPair.Value;
		for (TConstSetBitIterator<> It(Chunk.Bits); It; ++It)
		{
			const int32 LocalIndex = It.GetIndex();
			const int32 LocalX = LocalIndex % ChunkSize;
			const int32 LocalY = (LocalIndex / ChunkSize) % ChunkSize;
			const int32 LocalZ = LocalIndex / (ChunkSize * ChunkSize);
			const FIntVector Cell = ChunkOrigin + FIntVector(LocalX, LocalY, LocalZ);

			if (bFullyContained ||
				(Cell.X >= MinCell.X && Cell.X <= MaxCell.X &&
				 Cell.Y >= MinCell.Y && Cell.Y <= MaxCell.Y &&
				 Cell.Z >= MinCell.Z && Cell.Z <= MaxCell.Z))
			{
				OutCells.Add(Cell);
			}
		}
	}
}
