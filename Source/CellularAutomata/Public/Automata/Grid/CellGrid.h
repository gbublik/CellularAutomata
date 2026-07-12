#pragma once

#include "CoreMinimal.h"

/**
 * Абстрактное хранилище клеток клеточного автомата, адресуемых целыми
 * координатами сетки (FIntVector). Не знает, как клетки отрисовываются.
 */
class CELLULARAUTOMATA_API FCellGrid
{
public:
	explicit FCellGrid(float InCellSize)
		: CellSize(InCellSize)
	{
	}

	virtual ~FCellGrid() = default;

	FCellGrid(const FCellGrid&) = delete;
	FCellGrid& operator=(const FCellGrid&) = delete;

	virtual bool IsAlive(const FIntVector& Cell) const = 0;
	virtual void SetAlive(const FIntVector& Cell, bool bAlive) = 0;
	virtual void Clear() = 0;
	virtual int32 Num() const = 0;

	/** Возраст клетки - сколько поколений подряд она прожила (0 у только
	 *  что родившейся). Насыщающийся (не переполняется) - для клеток,
	 *  которых сейчас нет в сетке, возвращает 0. Используется отображением
	 *  клеток на материалы по возрасту (см. AAutomataOrchestrator::AgeMaterials). */
	virtual uint8 GetAge(const FIntVector& Cell) const = 0;
	virtual void SetAge(const FIntVector& Cell, uint8 Age) = 0;

	/** Угасающее ("Generations") состояние клетки - НЕ то же самое, что
	 *  Age выше: Age монотонно растёт, косметичен и осмыслен только для
	 *  живых клеток; угасание влияет на само правило (клетка в угасании
	 *  birth-immune, см. AAutomataOrchestrator::States) и осмыслено только
	 *  для НЕ живых клеток - отдельный канал хранения, не переиспользует
	 *  Age. Значения: 0 - не угасает (либо мертва совсем, либо жива - живая
	 *  клетка никогда не "угасает" одновременно), 2..(States-1) - стадия
	 *  угасания (см. CellDecay::AdvanceDecayStates() за точной схемой
	 *  переходов). Виртуальные с безопасным дефолтом ("угасание не
	 *  поддерживается этой реализацией грида") - тот же паттерн, что и у
	 *  GetOccupiedChunkCoords()/GetChunkWorldSize() ниже; при States == 2
	 *  (дефолт, классический бинарный автомат) эти методы не должны даже
	 *  вызываться на горячем пути (см. FCpuComputeStrategy::Step()'s
	 *  bDecayActive-гейт) - дефолт здесь чисто defensive. */
	virtual bool IsDecaying(const FIntVector& Cell) const { return false; }
	virtual void SetDecayState(const FIntVector& Cell, uint8 NewState) {}
	virtual uint8 GetDecayState(const FIntVector& Cell) const { return 0; }

	/** Как GetAliveCells(), но угасающие клетки (см. IsDecaying() выше) -
	 *  OutStates[i] - угасающее состояние (2..States-1) для OutCells[i],
	 *  тот же индекс. Нужен рендеру (AAutomataOrchestrator::BuildAgeBuckets())
	 *  для покраски угасающих клеток по стадии - GetAliveCells() их
	 *  сознательно не включает (означает строго "живая", см. её doc-comment
	 *  и IsAlive()), чтобы не задеть существующие потребители (выделение,
	 *  запекание, compute-кандидаты). Безопасный дефолт - пустой список. */
	virtual void GetDecayingCells(TArray<FIntVector>& OutCells, TArray<uint8>& OutStates) const
	{
		OutCells.Reset();
		OutStates.Reset();
	}

	/** Как GetDecayingCells(), но только внутри WorldBounds - тот же смысл,
	 *  что и GetAliveCellsInBounds() относительно GetAliveCells(). Наивный
	 *  дефолт (полный GetDecayingCells() + фильтр по клетке); FDenseCellGrid
	 *  переопределяет чанк-осведомлённым путём. */
	virtual void GetDecayingCellsInBounds(const FBox& WorldBounds, TArray<FIntVector>& OutCells, TArray<uint8>& OutStates) const
	{
		TArray<FIntVector> AllCells;
		TArray<uint8> AllStates;
		GetDecayingCells(AllCells, AllStates);

		OutCells.Reset();
		OutStates.Reset();
		for (int32 Index = 0; Index < AllCells.Num(); ++Index)
		{
			if (WorldBounds.IsInside(GridToWorld(AllCells[Index])))
			{
				OutCells.Add(AllCells[Index]);
				OutStates.Add(AllStates[Index]);
			}
		}
	}

	/** Заполняет OutCells координатами всех живых клеток (out-параметр,
	 *  чтобы не копировать внутреннее хранилище на каждый вызов). */
	virtual void GetAliveCells(TArray<FIntVector>& OutCells) const = 0;

	float GetCellSize() const { return CellSize; }

	/** Преобразование координат клетки в мировые координаты. Виртуальный,
	 *  т.к. зависит не только от CellSize, но и от топологии сетки
	 *  (кубическая, гексагональная и т.д.) - реализация по умолчанию
	 *  предполагает кубическую решётку. */
	virtual FVector GridToWorld(const FIntVector& Cell) const
	{
		return FVector(Cell.X, Cell.Y, Cell.Z) * CellSize;
	}

	/** Как GetAliveCells(), но только клетки, чей мировой GridToWorld()
	 *  попадает внутрь WorldBounds - используется рендером для отсечения
	 *  клеток вне ARenderCullVolume ДО построения FTransform/AddInstances
	 *  (см. AAutomataOrchestrator::BuildAgeBuckets()), а не после, как
	 *  CellCullStartDistance/CellCullEndDistance. Виртуальный с наивной
	 *  реализацией по умолчанию (полный GetAliveCells() + фильтр по
	 *  каждой клетке) - конкретные реализации со spatial-структурой (см.
	 *  FDenseCellGrid, которая может отбраковывать целые чанки) переопределяют
	 *  более дешёвым путём; тот же паттерн, что и у GridToWorld(). */
	virtual void GetAliveCellsInBounds(const FBox& WorldBounds, TArray<FIntVector>& OutCells) const
	{
		TArray<FIntVector> AllCells;
		GetAliveCells(AllCells);

		OutCells.Reset();
		for (const FIntVector& Cell : AllCells)
		{
			if (WorldBounds.IsInside(GridToWorld(Cell)))
			{
				OutCells.Add(Cell);
			}
		}
	}

	/** Координаты занятых чанков (не отдельных клеток) - для дешёвой
	 *  грубой геометрии (см. FChunkGridView/AAutomataOrchestrator::
	 *  RefreshGhostShape()), где один "кубик" на весь чанк, а не на
	 *  клетку. Виртуальный с наивным дефолтом (пустой список - "чанкинг
	 *  не поддерживается этой реализацией грида"), тот же паттерн, что и
	 *  GetAliveCellsInBounds(); FDenseCellGrid переопределяет дёшево -
	 *  просто отдаёт ключи уже существующей TMap<FIntVector, FChunk>, без
	 *  нового сканирования клеток. */
	virtual void GetOccupiedChunkCoords(TArray<FIntVector>& OutChunkCoords) const
	{
		OutChunkCoords.Reset();
	}

	/** Мировой размер одного чанка (ChunkSize * CellSize у FDenseCellGrid) -
	 *  0, если чанкинг не поддерживается (см. GetOccupiedChunkCoords()). */
	virtual float GetChunkWorldSize() const
	{
		return 0.0f;
	}

protected:
	float CellSize;
};
