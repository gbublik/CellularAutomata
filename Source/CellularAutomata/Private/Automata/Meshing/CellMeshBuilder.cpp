#include "Automata/Meshing/CellMeshBuilder.h"
#include "Automata/Grid/CellGrid.h"

namespace
{
	/** Описание одной из 6 граней куба: смещение к соседу, который её
	 *  закрывает, наружная нормаль и две оси плоскости грани. U/V подобраны
	 *  так, что Cross(U, V) == Normal - вместе с порядком вершин
	 *  (-U+V, +U+V, +U-V, -U-V) и треугольниками (0,1,3)/(1,2,3) это даёт
	 *  тот же winding, что у KismetProceduralMeshLibrary::GenerateBoxMesh(),
	 *  т.е. грань видна снаружи, а не изнутри. */
	struct FFaceDef
	{
		FIntVector NeighborOffset;
		FVector Normal;
		FVector U;
		FVector V;
	};

	const FFaceDef GFaces[6] =
	{
		{ FIntVector( 1,  0,  0), FVector( 1,  0,  0), FVector(0, 1, 0), FVector(0, 0, 1) },
		{ FIntVector(-1,  0,  0), FVector(-1,  0,  0), FVector(0, 0, 1), FVector(0, 1, 0) },
		{ FIntVector( 0,  1,  0), FVector( 0,  1,  0), FVector(0, 0, 1), FVector(1, 0, 0) },
		{ FIntVector( 0, -1,  0), FVector( 0, -1,  0), FVector(1, 0, 0), FVector(0, 0, 1) },
		{ FIntVector( 0,  0,  1), FVector( 0,  0,  1), FVector(1, 0, 0), FVector(0, 1, 0) },
		{ FIntVector( 0,  0, -1), FVector( 0,  0, -1), FVector(0, 1, 0), FVector(1, 0, 0) },
	};
}

int64 CellMeshBuilder::EstimateMeshBytes(int64 ExposedFaceCount)
{
	// 4 вершины на грань, 6 индексов. Считаем ОБЕ копии, которые существуют
	// одновременно на пике:
	//
	// 1) FCellMeshData - то, что строит эта функция: позиция, нормаль и UV,
	//    под LWC это 24+24+16 = 64 байта на вершину;
	// 2) FProcMeshSection внутри UProceduralMeshComponent - туда
	//    CreateMeshSection_LinearColor() всё КОПИРУЕТ, а её FProcMeshVertex
	//    заметно толще: позиция 24, нормаль 24, тангент 32, цвет 4 и ЧЕТЫРЕ
	//    канала UV по 16 - около 152 байт, причём тангенты, цвет и три UV из
	//    четырёх мы не используем вовсе, структура просто фиксированная.
	//
	// Учитывать только первую было ошибкой: она занижала пик в 3.4 раза, и
	// бюджет означал совсем не то число, которое в нём написано. Буферы на
	// стороне GPU сверх этого не учитываются - они появляются позже и живут
	// уже без наших массивов.
	const int64 BuildBytesPerVertex = sizeof(FVector) + sizeof(FVector) + sizeof(FVector2D);
	const int64 SectionBytesPerVertex = 152;
	const int64 BytesPerVertex = BuildBytesPerVertex + SectionBytesPerVertex;
	// Индексы тоже в двух копиях - наш TArray<int32> и буфер секции.
	return ExposedFaceCount * (4 * BytesPerVertex + 2 * 6 * (int64)sizeof(int32));
}

int64 CellMeshBuilder::CountExposedFaces(const FCellGrid& Grid, const TArray<FIntVector>& Cells, bool bUseGridMembership)
{
	// TSet строится только когда без него нельзя (бейк подмножества) - см.
	// doc-comment BuildFromCells().
	TSet<FIntVector> CellSet;
	if (!bUseGridMembership)
	{
		CellSet.Append(Cells);
	}

	int64 ExposedFaceCount = 0;
	for (const FIntVector& Cell : Cells)
	{
		for (const FFaceDef& Face : GFaces)
		{
			const FIntVector Neighbor = Cell + Face.NeighborOffset;
			const bool bNeighborPresent = bUseGridMembership ? Grid.IsAlive(Neighbor) : CellSet.Contains(Neighbor);
			if (!bNeighborPresent)
			{
				++ExposedFaceCount;
			}
		}
	}

	return ExposedFaceCount;
}

CellMeshBuilder::FCellMeshData CellMeshBuilder::BuildFromCells(const FCellGrid& Grid, const TArray<FIntVector>& Cells, bool bUseGridMembership)
{
	FCellMeshData MeshData;

	TSet<FIntVector> CellSet;
	if (!bUseGridMembership)
	{
		CellSet.Append(Cells);
	}
	// Полугабарит ячейки ПО КАЖДОЙ ОСИ, а не одно число: на решётке с
	// неравным шагом (и в масштабе чанков у FChunkGridView) ячейка - коробка,
	// а не куб. Оси граней в GFaces единичные, поэтому покомпонентное
	// умножение ниже сразу даёт нужную полудлину вдоль каждой из них.
	const FVector HalfExtent = Grid.GetLattice().GetCellWorldExtent() * 0.5;

	// Первый проход - точный подсчёт наружных граней для Reserve: при
	// миллионах клеток многократные переаллокации массивов вершин дороже,
	// чем лишний прогон дешёвых проверок соседей.
	const int64 ExposedFaceCount = CountExposedFaces(Grid, Cells, bUseGridMembership);

	MeshData.Vertices.Reserve(ExposedFaceCount * 4);
	MeshData.Normals.Reserve(ExposedFaceCount * 4);
	MeshData.UVs.Reserve(ExposedFaceCount * 4);
	MeshData.Triangles.Reserve(ExposedFaceCount * 6);

	for (const FIntVector& Cell : Cells)
	{
		const FVector Center = Grid.GridToWorld(Cell);

		for (const FFaceDef& Face : GFaces)
		{
			const FIntVector Neighbor = Cell + Face.NeighborOffset;
			const bool bNeighborPresent = bUseGridMembership ? Grid.IsAlive(Neighbor) : CellSet.Contains(Neighbor);
			if (bNeighborPresent)
			{
				continue;
			}

			const FVector FaceCenter = Center + Face.Normal * HalfExtent;
			const FVector U = Face.U * HalfExtent;
			const FVector V = Face.V * HalfExtent;

			const int32 BaseIndex = MeshData.Vertices.Num();
			MeshData.Vertices.Add(FaceCenter - U + V);
			MeshData.Vertices.Add(FaceCenter + U + V);
			MeshData.Vertices.Add(FaceCenter + U - V);
			MeshData.Vertices.Add(FaceCenter - U - V);

			MeshData.Normals.Add(Face.Normal);
			MeshData.Normals.Add(Face.Normal);
			MeshData.Normals.Add(Face.Normal);
			MeshData.Normals.Add(Face.Normal);

			MeshData.UVs.Add(FVector2D(0.0, 1.0));
			MeshData.UVs.Add(FVector2D(1.0, 1.0));
			MeshData.UVs.Add(FVector2D(1.0, 0.0));
			MeshData.UVs.Add(FVector2D(0.0, 0.0));

			MeshData.Triangles.Add(BaseIndex + 0);
			MeshData.Triangles.Add(BaseIndex + 1);
			MeshData.Triangles.Add(BaseIndex + 3);
			MeshData.Triangles.Add(BaseIndex + 1);
			MeshData.Triangles.Add(BaseIndex + 2);
			MeshData.Triangles.Add(BaseIndex + 3);
		}
	}

	return MeshData;
}
