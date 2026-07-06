#pragma once

#include "CoreMinimal.h"

class FCellGrid;

/** Построение цельного меша из набора клеток - только наружные грани
 *  (классический face culling: грань куба генерируется, только если соседней
 *  клетки НЕТ в наборе, внутренность полая). Plain namespace-функция, не
 *  UObject - та же идиома, что CellAging/CellSelection. Используется
 *  AAutomataOrchestrator::BakeCellsToMesh() (хоткей M): снимок-"скульптура"
 *  текущего состояния одним мешом вместо миллионов инстансов-кубиков. */
namespace CellMeshBuilder
{
	/** Готовые массивы под UProceduralMeshComponent::CreateMeshSection_LinearColor().
	 *  Вершины НЕ шарятся между гранями (4 на грань, у каждой нормаль своей
	 *  грани) - иначе нормали на рёбрах куба усреднялись бы и рёбра "плыли". */
	struct FCellMeshData
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
	};

	/** Grid нужен только для GridToWorld()/GetCellSize() (координаты и размер
	 *  куба клетки); принадлежность клетки набору проверяется по Cells, а не
	 *  по Grid->IsAlive() - так запекание выделенного подмножества не рисует
	 *  внутренние грани на границе с невыделенными-но-живыми соседями. */
	CELLULARAUTOMATA_API FCellMeshData BuildFromCells(const FCellGrid& Grid, const TArray<FIntVector>& Cells);
}
