#include "Automata/Grid/DenseCellGrid.h"

namespace
{
	/** Ближайшая сверху степень двойки, не меньше 1. См. RoundedChunkSize()
	 *  ниже за тем, зачем размер чанка обязан быть степенью двойки. */
	FORCEINLINE int32 RoundUpToPowerOfTwo(int32 Value)
	{
		return (Value <= 1) ? 1 : static_cast<int32>(1u << FMath::CeilLogTwo(static_cast<uint32>(Value)));
	}
}

int32 FDenseCellGrid::RoundedChunkSize(int32 RequestedChunkSize)
{
	const int32 Rounded = RoundUpToPowerOfTwo(RequestedChunkSize);
	if (Rounded != RequestedChunkSize)
	{
		UE_LOG(LogTemp, Warning, TEXT("FDenseCellGrid: размер чанка %d округлён до %d - он обязан быть степенью двойки (см. CellToChunkCoord())"),
			RequestedChunkSize, Rounded);
	}
	return Rounded;
}

FDenseCellGrid::FDenseCellGrid(float InCellSize, int32 InChunkSize, bool bInEnableDecayStates)
	: FDenseCellGrid(FLatticeTransform::MakeOrthogonal(InCellSize), InChunkSize, bInEnableDecayStates)
{
}

FDenseCellGrid::FDenseCellGrid(const FLatticeTransform& InLattice, int32 InChunkSize, bool bInEnableDecayStates)
	: FCellGrid(InLattice)
	, ChunkSize(RoundedChunkSize(InChunkSize))
	, ChunkShift(static_cast<int32>(FMath::FloorLog2(static_cast<uint32>(ChunkSize))))
	, ChunkMask(ChunkSize - 1)
	, CellsPerChunk(ChunkSize * ChunkSize * ChunkSize)
	, bDecayStatesEnabled(bInEnableDecayStates)
{
}

FIntVector FDenseCellGrid::CellToChunkCoord(const FIntVector& Cell) const
{
	// Арифметический сдвиг вправо - это и есть floor-деление на степень
	// двойки, включая отрицательные координаты: -1 >> 4 == -1, -17 >> 4 ==
	// -2, что ровно floor(-1/16) и floor(-17/16). Это принципиально важно
	// здесь - генерация клеток центрирована в нуле, так что отрицательные
	// координаты не исключение, а норма.
	//
	// Раньше тут звался написанный вручную FloorDiv() - обычное деление
	// усекает к нулю, а FMath::DivideAndRoundDown вопреки названию делает
	// то же самое, так что оба давали бы 0 вместо -1 на первой же клетке
	// слева от начала координат. Ловушка никуда не делась, просто теперь её
	// снимает сама разрядная сетка, а не ветвление: девять целочисленных
	// делений на вызов (тут и в CellToLocalIndex()) складывались примерно в
	// половину стоимости SetAlive(), а он - главный расход всего проекта
	// (см. FindChunkForWrite()).
	return FIntVector(
		Cell.X >> ChunkShift,
		Cell.Y >> ChunkShift,
		Cell.Z >> ChunkShift);
}

int32 FDenseCellGrid::CellToLocalIndex(const FIntVector& Cell) const
{
	// & ChunkMask - это положительный остаток по степени двойки, тоже без
	// поправки на знак: -1 & 15 == 15, что и требуется (оператор % вернул бы
	// -1). Пара к сдвигу в CellToChunkCoord(), см. комментарий там.
	const int32 LocalX = Cell.X & ChunkMask;
	const int32 LocalY = Cell.Y & ChunkMask;
	const int32 LocalZ = Cell.Z & ChunkMask;
	return ((((LocalZ << ChunkShift) + LocalY) << ChunkShift) + LocalX);
}

FIntVector FDenseCellGrid::LocalIndexToOffset(int32 LocalIndex) const
{
	// Обратная к CellToLocalIndex(): раскладывает плоский индекс внутри
	// чанка обратно в смещение по осям. Здесь координаты заведомо
	// неотрицательны (индекс в пределах чанка), так что ловушки со знаком
	// нет - только замена деления на сдвиг, как и в прямом преобразовании.
	// Вынесено в общий метод: до этого одна и та же тройка строк была
	// скопирована в четырёх местах перечисления клеток.
	return FIntVector(
		LocalIndex & ChunkMask,
		(LocalIndex >> ChunkShift) & ChunkMask,
		LocalIndex >> (ChunkShift * 2));
}

FDenseCellGrid::FChunk* FDenseCellGrid::FindChunkForWrite(const FIntVector& ChunkCoord)
{
	if (bChunkCacheValid && CachedChunkCoord == ChunkCoord)
	{
		return CachedChunk;
	}

	FChunk* Chunk = Chunks.Find(ChunkCoord);
	CacheChunk(ChunkCoord, Chunk);
	return Chunk;
}

void FDenseCellGrid::CacheChunk(const FIntVector& ChunkCoord, FChunk* Chunk)
{
	CachedChunkCoord = ChunkCoord;
	CachedChunk = Chunk;
	bChunkCacheValid = true;
}

void FDenseCellGrid::InvalidateChunkCache()
{
	CachedChunk = nullptr;
	bChunkCacheValid = false;
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
		FChunk* Chunk = FindChunkForWrite(ChunkCoord);
		if (!Chunk)
		{
			// Ленивое создание чанка - только когда в нём появляется
			// первая живая клетка.
			Chunk = &Chunks.Add(ChunkCoord, FChunk(CellsPerChunk, bDecayStatesEnabled));
			// Обязательно: Add мог перевыделить TMap и сдвинуть элементы,
			// так что закешированный ранее указатель уже мог повиснуть -
			// перезаписываем его только что добавленным (см. CacheChunk()).
			CacheChunk(ChunkCoord, Chunk);
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
		FChunk* Chunk = FindChunkForWrite(ChunkCoord);
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
				InvalidateChunkCache();
			}
		}
	}
}

void FDenseCellGrid::SetAliveWithAge(const FIntVector& Cell, uint8 Age)
{
	const FIntVector ChunkCoord = CellToChunkCoord(Cell);
	const int32 LocalIndex = CellToLocalIndex(Cell);

	FChunk* Chunk = FindChunkForWrite(ChunkCoord);
	if (!Chunk)
	{
		// Ленивое создание чанка - как в SetAlive(true), включая обязательную
		// перезапись кеша после Add (см. комментарий там).
		Chunk = &Chunks.Add(ChunkCoord, FChunk(CellsPerChunk, bDecayStatesEnabled));
		CacheChunk(ChunkCoord, Chunk);
	}

	if (!Chunk->Bits[LocalIndex])
	{
		Chunk->Bits[LocalIndex] = true;
		++Chunk->AliveCount;
		++AliveCellCount;
	}

	// Пишется безусловно, без проверки на ноль: чанк создаётся с обнулёнными
	// возрастами, так что запись нуля в ноль ничего не меняет, а ветвление
	// на горячем пути стоит дороже, чем сам байт.
	Chunk->Ages[LocalIndex] = Age;
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
		FChunk* Chunk = FindChunkForWrite(ChunkCoord);
		if (!Chunk)
		{
			// Лениво создаём чанк - зеркалит SetAlive(true)'s ленивое
			// создание. NextGrid каждое поколение пуст с нуля (двойная
			// буферизация), так что только что умершая/угасающая клетка
			// нередко требует создать чанк здесь, а не найти уже
			// существующий.
			Chunk = &Chunks.Add(ChunkCoord, FChunk(CellsPerChunk, bDecayStatesEnabled));
			// См. одноимённый комментарий в SetAlive().
			CacheChunk(ChunkCoord, Chunk);
		}

		if (Chunk->DecayStates[LocalIndex] == 0)
		{
			++Chunk->DecayingCount;
		}
		Chunk->DecayStates[LocalIndex] = NewState;
	}
	else
	{
		FChunk* Chunk = FindChunkForWrite(ChunkCoord);
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
				InvalidateChunkCache();
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
				OutCells.Add(ChunkOrigin + LocalIndexToOffset(LocalIndex));
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
	FIntVector MinCell, MaxCell;
	Lattice.WorldBoundsToCellRange(WorldBounds, MinCell, MaxCell);

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

			const FIntVector Cell = ChunkOrigin + LocalIndexToOffset(LocalIndex);

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
	InvalidateChunkCache();
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
			OutCells.Add(ChunkOrigin + LocalIndexToOffset(LocalIndex));
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

	// WorldBounds -> границы в клеточном пространстве. Рамка намеренно
	// консервативна (floor/ceil), чтобы ни одна частично захваченная клетка
	// не потерялась на краю - сама по-клеточная проверка ниже, для граничных
	// чанков, уже точная. Раньше деление на CellSize стояло здесь открытым
	// текстом и было одним из четырёх ручных обратных преобразований в
	// проекте; теперь оно живёт в решётке и потому одинаково для любого шага
	// по осям (см. FLatticeTransform).
	FIntVector MinCell, MaxCell;
	Lattice.WorldBoundsToCellRange(WorldBounds, MinCell, MaxCell);

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
			const FIntVector Cell = ChunkOrigin + LocalIndexToOffset(LocalIndex);

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
