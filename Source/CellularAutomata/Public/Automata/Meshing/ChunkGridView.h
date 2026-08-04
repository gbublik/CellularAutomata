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
 *  Поправка на центр чанка - не мелочь: наивное Чанк*РазмерЧанка ставило бы
 *  куб чанка центром в его угол, тогда как настоящий центр лежит на полчанка
 *  дальше минус полклетки, потому что GridToWorld() у клеток даёт их ЦЕНТРЫ,
 *  а не углы. Чанк C занимает клетки от C*ChunkSize до C*ChunkSize+ChunkSize-1,
 *  то есть в мире тянется от C*РазмерЧанка - Клетка/2 до
 *  C*РазмерЧанка + РазмерЧанка - Клетка/2. Без неё ghost-силуэт уезжает на
 *  (РазмерЧанка - Клетка)/2 по каждой оси - при ChunkSize=16 и CellSize=100
 *  это 750 юнитов, что и наблюдалось живьём как зазор между силуэтом и
 *  детальными клетками.
 *
 *  Раньше ради этого переопределялся GridToWorld(), а поправка хранилась
 *  отдельным скалярным полем. Теперь она - Origin решётки чанков
 *  (FLatticeTransform::MakeChunkView()), override исчез, и поправка стала
 *  ПОКОМПОНЕНТНОЙ: на решётке с неравным шагом по осям скаляр был бы верен
 *  только по одной из них, а силуэт разъезжался бы по остальным.
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
	/** ChunkWorldExtent/CellWorldExtent - мировые габариты чанка и клетки по
	 *  каждой оси (Grid->GetChunkWorldExtent() и
	 *  Grid->GetLattice().GetCellWorldExtent()). Решётка этой вьюхи - шаг в
	 *  размер чанка со сдвигом в его центр, поэтому базовый GridToWorld()
	 *  сразу даёт что надо и переопределять его не требуется. */
	FChunkGridView(const FVector& InChunkWorldExtent, const FVector& InCellWorldExtent, TArray<FIntVector> InOccupiedChunkCoords, bool bBuildOccupancySet = false)
		: FCellGrid(FLatticeTransform(InChunkWorldExtent, (InChunkWorldExtent - InCellWorldExtent) * 0.5))
		, OccupiedChunkCoords(MoveTemp(InOccupiedChunkCoords))
	{
		if (bBuildOccupancySet)
		{
			OccupiedChunkSet.Append(OccupiedChunkCoords);
		}
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
	TArray<FIntVector> OccupiedChunkCoords;

	/** Пустое, если конструктор звали без bBuildOccupancySet - см. IsAlive(). */
	TSet<FIntVector> OccupiedChunkSet;
};
