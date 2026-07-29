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

	/** Сколько байт займёт FCellMeshData на такое число наружных граней -
	 *  4 вершины на грань (позиция + нормаль + UV) плюс 6 индексов.
	 *  Нужна вызывающему, чтобы оценить бейк ДО того, как начать его строить:
	 *  на пористых структурах (салфетка Серпинского - 13% заполнения, у почти
	 *  каждой клетки грани открыты) выходит 2-4 грани на клетку, то есть
	 *  порядка 800 байт против 108 у инстанса, и бейк съедает памяти в разы
	 *  больше, чем то, что он заменяет. Наблюдалось: M на большой сетке
	 *  выбирал всю оперативную память и вешал редактор. */
	CELLULARAUTOMATA_API int64 EstimateMeshBytes(int64 ExposedFaceCount);

	/** Точное число наружных граней - тот же проход, что делает
	 *  BuildFromCells() перед Reserve, но без единой аллокации под геометрию.
	 *  Отделён именно ради проверки бюджета до начала работы. */
	CELLULARAUTOMATA_API int64 CountExposedFaces(const FCellGrid& Grid, const TArray<FIntVector>& Cells, bool bUseGridMembership);

	/** Grid нужен для GridToWorld()/GetCellSize() (координаты и размер куба
	 *  клетки), а при bUseGridMembership - ещё и для проверки соседей.
	 *
	 *  bUseGridMembership == false: принадлежность проверяется по Cells через
	 *  временный TSet - так запекание ВЫДЕЛЕННОГО подмножества не рисует
	 *  внутренние грани на границе с невыделенными-но-живыми соседями.
	 *
	 *  bUseGridMembership == true: сосед проверяется прямо через
	 *  Grid.IsAlive(), и TSet не строится вовсе. Годится только когда Cells -
	 *  это ВСЕ живые клетки сетки (ответ тогда тот же самый), зато экономит
	 *  сотни мегабайт: TSet на 19 млн клеток это под гигабайт впустую. Именно
	 *  этот случай - бейк без выделения - и есть тот, на котором память
	 *  кончается. */
	CELLULARAUTOMATA_API FCellMeshData BuildFromCells(const FCellGrid& Grid, const TArray<FIntVector>& Cells, bool bUseGridMembership = false);
}
