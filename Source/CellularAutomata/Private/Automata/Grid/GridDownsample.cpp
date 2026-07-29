#include "Automata/Grid/GridDownsample.h"
#include "Automata/Grid/DenseCellGrid.h"

TUniquePtr<FDenseCellGrid> GridDownsample::Downsample(const FCellGrid& Source, const TArray<FIntVector>& Cells, int32 Factor, int32 ChunkSize)
{
	// Степень двойки - чтобы деление координаты было сдвигом. Для
	// отрицательных координат (а генерация центрирована в нуле, так что они
	// норма) арифметический сдвиг вправо и есть floor-деление: -1 >> 1 == -1,
	// что верно, тогда как -1 / 2 дало бы 0 и склеило бы две разные крупные
	// клетки в одну. Тот же приём и та же ловушка, что в
	// FDenseCellGrid::CellToChunkCoord().
	const int32 SafeFactor = (Factor <= 1) ? 1 : (int32)FMath::RoundUpToPowerOfTwo((uint32)Factor);
	const int32 Shift = FMath::FloorLog2((uint32)SafeFactor);

	TUniquePtr<FDenseCellGrid> Coarse = MakeUnique<FDenseCellGrid>(Source.GetCellSize() * SafeFactor, ChunkSize);

	// "Хоть одна живая" получается само собой: повторные SetAlive() по той же
	// крупной координате идемпотентны, никакого подсчёта голосов не нужно.
	// Клетки приходят из GetAliveCells() в порядке чанков, то есть
	// пространственно связно, так что кеш чанка в приёмнике почти всегда
	// попадает (см. FDenseCellGrid::FindChunkForWrite()).
	for (const FIntVector& Cell : Cells)
	{
		Coarse->SetAlive(FIntVector(Cell.X >> Shift, Cell.Y >> Shift, Cell.Z >> Shift), true);
	}

	return Coarse;
}
