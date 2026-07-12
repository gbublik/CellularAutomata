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

FDenseCellGrid::FDenseCellGrid(float InCellSize, int32 InChunkSize, bool bInEnableDecayStates)
	: FCellGrid(InCellSize)
	, ChunkSize(FMath::Max(1, InChunkSize))
	, CellsPerChunk(ChunkSize * ChunkSize * ChunkSize)
	, bDecayStatesEnabled(bInEnableDecayStates)
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
			Chunk = &Chunks.Add(ChunkCoord, FChunk(CellsPerChunk, bDecayStatesEnabled));
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

			// Чанк удаляется, только если в нём не осталось ни живых, ни
			// угасающих клеток (см. FChunk::DecayingCount) - иначе чанк с
			// одними угасающими клетками (уже не живыми, но ещё не
			// полностью мёртвыми) удалился бы раньше времени, теряя их
			// состояние угасания.
			if (Chunk->AliveCount == 0 && Chunk->DecayingCount == 0)
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

bool FDenseCellGrid::IsDecaying(const FIntVector& Cell) const
{
	// Гейт первой строкой - при States==2 (канал выключен) ни один
	// Chunks.Find() не выполняется, см. doc-comment FCellGrid::IsDecaying().
	if (!bDecayStatesEnabled)
	{
		return false;
	}

	const FChunk* Chunk = Chunks.Find(CellToChunkCoord(Cell));
	return Chunk && Chunk->DecayStates[CellToLocalIndex(Cell)] != 0;
}

uint8 FDenseCellGrid::GetDecayState(const FIntVector& Cell) const
{
	if (!bDecayStatesEnabled)
	{
		return 0;
	}

	const FChunk* Chunk = Chunks.Find(CellToChunkCoord(Cell));
	return Chunk ? Chunk->DecayStates[CellToLocalIndex(Cell)] : 0;
}

void FDenseCellGrid::SetDecayState(const FIntVector& Cell, uint8 NewState)
{
	if (!bDecayStatesEnabled)
	{
		return;
	}

	const FIntVector ChunkCoord = CellToChunkCoord(Cell);
	const int32 LocalIndex = CellToLocalIndex(Cell);

	if (NewState != 0)
	{
		FChunk* Chunk = Chunks.Find(ChunkCoord);
		if (!Chunk)
		{
			// Лениво создаём чанк - зеркалит SetAlive(true)'s ленивое
			// создание. NextGrid каждое поколение пуст с нуля (двойная
			// буферизация), так что только что умершая/угасающая клетка
			// нередко требует создать чанк здесь, а не найти уже
			// существующий.
			Chunk = &Chunks.Add(ChunkCoord, FChunk(CellsPerChunk, bDecayStatesEnabled));
		}

		if (Chunk->DecayStates[LocalIndex] == 0)
		{
			++Chunk->DecayingCount;
		}
		Chunk->DecayStates[LocalIndex] = NewState;
	}
	else
	{
		FChunk* Chunk = Chunks.Find(ChunkCoord);
		if (!Chunk)
		{
			return;
		}

		if (Chunk->DecayStates[LocalIndex] != 0)
		{
			Chunk->DecayStates[LocalIndex] = 0;
			--Chunk->DecayingCount;

			// Та же защита от преждевременного удаления, что и в
			// SetAlive(false) - чанк удаляется, только если не осталось ни
			// живых, ни угасающих клеток.
			if (Chunk->AliveCount == 0 && Chunk->DecayingCount == 0)
			{
				Chunks.Remove(ChunkCoord);
			}
		}
	}
}

void FDenseCellGrid::GetDecayingCells(TArray<FIntVector>& OutCells, TArray<uint8>& OutStates) const
{
	OutCells.Reset();
	OutStates.Reset();

	if (!bDecayStatesEnabled)
	{
		return;
	}

	for (const TPair<FIntVector, FChunk>& ChunkPair : Chunks)
	{
		const FChunk& Chunk = ChunkPair.Value;
		if (Chunk.DecayingCount == 0)
		{
			// Дёшево пропускаем чанки без угасающих клеток вовсе - не
			// сканируем DecayStates целиком (в отличие от Bits, DecayStates
			// не битовый массив, TConstSetBitIterator недоступен).
			continue;
		}

		const FIntVector ChunkOrigin = ChunkPair.Key * ChunkSize;
		for (int32 LocalIndex = 0; LocalIndex < CellsPerChunk; ++LocalIndex)
		{
			const uint8 State = Chunk.DecayStates[LocalIndex];
			if (State != 0)
			{
				const int32 LocalX = LocalIndex % ChunkSize;
				const int32 LocalY = (LocalIndex / ChunkSize) % ChunkSize;
				const int32 LocalZ = LocalIndex / (ChunkSize * ChunkSize);

				OutCells.Add(ChunkOrigin + FIntVector(LocalX, LocalY, LocalZ));
				OutStates.Add(State);
			}
		}
	}
}

void FDenseCellGrid::GetDecayingCellsInBounds(const FBox& WorldBounds, TArray<FIntVector>& OutCells, TArray<uint8>& OutStates) const
{
	OutCells.Reset();
	OutStates.Reset();

	if (!bDecayStatesEnabled)
	{
		return;
	}

	// Та же клеточно-пространственная рамка и та же двухуровневая проверка
	// (bOverlaps/bFullyContained), что и GetAliveCellsInBounds() - см. её
	// комментарии за подробностями.
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
		const FChunk& Chunk = ChunkPair.Value;
		if (Chunk.DecayingCount == 0)
		{
			continue;
		}

		const FIntVector ChunkOrigin = ChunkPair.Key * ChunkSize;
		const FIntVector ChunkMax = ChunkOrigin + FIntVector(ChunkSize - 1, ChunkSize - 1, ChunkSize - 1);

		const bool bOverlaps =
			ChunkOrigin.X <= MaxCell.X && ChunkMax.X >= MinCell.X &&
			ChunkOrigin.Y <= MaxCell.Y && ChunkMax.Y >= MinCell.Y &&
			ChunkOrigin.Z <= MaxCell.Z && ChunkMax.Z >= MinCell.Z;
		if (!bOverlaps)
		{
			continue;
		}

		const bool bFullyContained =
			ChunkOrigin.X >= MinCell.X && ChunkMax.X <= MaxCell.X &&
			ChunkOrigin.Y >= MinCell.Y && ChunkMax.Y <= MaxCell.Y &&
			ChunkOrigin.Z >= MinCell.Z && ChunkMax.Z <= MaxCell.Z;

		for (int32 LocalIndex = 0; LocalIndex < CellsPerChunk; ++LocalIndex)
		{
			const uint8 State = Chunk.DecayStates[LocalIndex];
			if (State == 0)
			{
				continue;
			}

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
				OutStates.Add(State);
			}
		}
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

void FDenseCellGrid::GetOccupiedChunkCoords(TArray<FIntVector>& OutChunkCoords) const
{
	OutChunkCoords.Reset(Chunks.Num());
	for (const TPair<FIntVector, FChunk>& ChunkPair : Chunks)
	{
		OutChunkCoords.Add(ChunkPair.Key);
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
