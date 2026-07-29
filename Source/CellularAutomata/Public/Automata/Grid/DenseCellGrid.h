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

	/** bInEnableDecayStates=false (дефолт) - классический бинарный автомат,
	 *  DecayStates ни у одного чанка не аллоцируется (см. FChunk ниже) -
	 *  ровно сегодняшнее поведение, ноль лишней памяти. true - включает
	 *  канал угасания (см. AAutomataOrchestrator::States > 2), вызывается
	 *  из CreateGrid() как States > 2. */
	explicit FDenseCellGrid(float InCellSize, int32 InChunkSize = DefaultChunkSize, bool bInEnableDecayStates = false);
	virtual ~FDenseCellGrid() override = default;

	virtual bool IsAlive(const FIntVector& Cell) const override;
	virtual void SetAlive(const FIntVector& Cell, bool bAlive) override;
	virtual void Clear() override;
	virtual int32 Num() const override;
	virtual void GetAliveCells(TArray<FIntVector>& OutCells) const override;
	virtual uint8 GetAge(const FIntVector& Cell) const override;
	virtual void SetAge(const FIntVector& Cell, uint8 Age) override;

	/** Первой строкой проверяет bDecayStatesEnabled - при States==2 (канал
	 *  выключен) это единственная работа, ни один TMap::Find() не
	 *  выполняется (см. doc-comment FCellGrid::IsDecaying()). */
	virtual bool IsDecaying(const FIntVector& Cell) const override;
	virtual void SetDecayState(const FIntVector& Cell, uint8 NewState) override;
	virtual uint8 GetDecayState(const FIntVector& Cell) const override;
	virtual void GetDecayingCells(TArray<FIntVector>& OutCells, TArray<uint8>& OutStates) const override;
	virtual void GetDecayingCellsInBounds(const FBox& WorldBounds, TArray<FIntVector>& OutCells, TArray<uint8>& OutStates) const override;

	/** Чанк-осведомлённый override - отбраковывает целиком чанки, не
	 *  пересекающие WorldBounds (ни один бит не читается), и разбирает
	 *  побитово только реально пересекающиеся; для чанков, целиком
	 *  лежащих внутри границ, пропускает поклеточную проверку (как
	 *  обычный GetAliveCells()). См. doc-comment в FCellGrid. */
	virtual void GetAliveCellsInBounds(const FBox& WorldBounds, TArray<FIntVector>& OutCells) const override;

	/** Дёшево - Chunks уже хранит нужные ключи, никакого нового
	 *  сканирования клеток (см. doc-comment в FCellGrid). */
	virtual void GetOccupiedChunkCoords(TArray<FIntVector>& OutChunkCoords) const override;
	virtual float GetChunkWorldSize() const override { return ChunkSize * CellSize; }

private:
	/** Плотный чанк ChunkSize^3 клеток. AliveCount кэширует число живых
	 *  бит, чтобы SetAlive(false) мог дёшево (O(1)) определить, опустел
	 *  ли чанк, не сканируя битовый массив целиком. Ages - параллельный
	 *  битовому массиву возраст каждой клетки (тот же LocalIndex, что и
	 *  Bits), нулевой по умолчанию - зануляется как при ленивом создании
	 *  чанка, так и (полностью, вместе со всем чанком) при опустошении,
	 *  так что заново родившаяся в этом месте клетка всегда стартует с
	 *  возраста 0, не помня историю чанка до его удаления.
	 *
	 *  DecayStates - третий параллельный канал (угасающее "Generations"-
	 *  состояние, см. FCellGrid::IsDecaying()) - РЕАЛЬНО аллоцируется
	 *  (SetNumZeroed) только когда bInEnableDecayStates true; при false
	 *  остаётся дефолтно-пустым TArray без выделения кучи - при States==2
	 *  (подавляющее большинство сценариев) это буквально нулевая
	 *  дополнительная память на чанк, только сам факт существования поля
	 *  (несколько байт заголовка TArray, тот же порядок величины, что уже
	 *  есть у Bits/Ages). DecayingCount - тот же смысл, что AliveCount, но
	 *  для угасающих (не живых, но ещё не полностью мёртвых) клеток -
	 *  используется отсечением опустевших чанков (см. SetAlive(false)/
	 *  SetDecayState() в .cpp): чанк с одними угасающими клетками не
	 *  должен удаляться, даже если AliveCount уже 0. */
	struct FChunk
	{
		FChunk(int32 InCellsPerChunk, bool bInEnableDecayStates)
		{
			Bits.Init(false, InCellsPerChunk);
			Ages.SetNumZeroed(InCellsPerChunk);
			if (bInEnableDecayStates)
			{
				DecayStates.SetNumZeroed(InCellsPerChunk);
			}
		}

		TBitArray<> Bits;
		TArray<uint8> Ages;
		TArray<uint8> DecayStates;
		int32 AliveCount = 0;
		int32 DecayingCount = 0;
	};

	FIntVector CellToChunkCoord(const FIntVector& Cell) const;
	int32 CellToLocalIndex(const FIntVector& Cell) const;

	/** Chunks.Find() для ПУТИ ЗАПИСИ, с кешем последнего чанка. Именно этот
	 *  поиск оказался главным узким местом всего проекта: замерами он вышел
	 *  в ~20-24 нс на вызов (хеш плюс промах мимо кеша на большой TMap), и
	 *  из него целиком складываются и Unpack у FGpuComputeStrategy, и
	 *  WriteBack у FCpuComputeStrategy - 806 мс из 1111 мс шага на 35 млн
	 *  клеток. Обе фазы пишут клетки пространственно связными пробегами, так
	 *  что подряд идущие вызовы почти всегда попадают в тот же чанк, и один
	 *  сохранённый указатель убирает подавляющее большинство поисков.
	 *
	 *  ВАЖНО - вызывать только оттуда, где запись заведомо последовательна.
	 *  Кеш это разделяемое изменяемое состояние без всякой синхронизации,
	 *  поэтому он НАМЕРЕННО не используется ни в одном const-методе чтения
	 *  (IsAlive() молотится из ParallelFor в FCpuComputeStrategy), ни в
	 *  SetAge() (CellAging::ComputeAges() параллелен по построению - см. его
	 *  doc-comment о том, почему именно SetAge безопасно звать из потоков).
	 *  Сейчас законные вызывающие - SetAlive() и SetDecayState(), обе
	 *  последовательны по той же причине, по которой последователен
	 *  WriteBack: TBitArray не потокобезопасен на запись.
	 *
	 *  Возвращает nullptr, если чанка нет - отрицательный результат тоже
	 *  кешируется (повторные промахи по одной координате не платят за
	 *  Find()). */
	FChunk* FindChunkForWrite(const FIntVector& ChunkCoord);

	/** Запомнить чанк как последний записанный. Звать ОБЯЗАТЕЛЬНО после
	 *  каждого Chunks.Add(): Add может перевыделить внутренний массив TMap и
	 *  сдвинуть все элементы, после чего ранее сохранённый указатель
	 *  повиснет. Перезапись кеша только что добавленным чанком снимает эту
	 *  опасность - никакого другого указателя мы не держим. */
	void CacheChunk(const FIntVector& ChunkCoord, FChunk* Chunk);

	/** Сбросить кеш - после Chunks.Remove()/Empty(), т.е. когда сохранённый
	 *  указатель мог перестать быть валидным. */
	void InvalidateChunkCache();

	int32 ChunkSize;
	int32 CellsPerChunk; // ChunkSize^3, кэшировано
	bool bDecayStatesEnabled;
	TMap<FIntVector, FChunk> Chunks;
	int32 AliveCellCount = 0;

	/** См. FindChunkForWrite(). Отдельный флаг валидности, а не часовое
	 *  значение в координате: CachedChunk == nullptr - это законное
	 *  закешированное "чанка по такой координате нет", и отличить его от
	 *  пустого кеша иначе было бы нечем. */
	FIntVector CachedChunkCoord = FIntVector::ZeroValue;
	FChunk* CachedChunk = nullptr;
	bool bChunkCacheValid = false;
};
