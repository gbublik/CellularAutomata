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

CellMeshBuilder::FCellMeshData CellMeshBuilder::BuildFromCells(const FCellGrid& Grid, const TArray<FIntVector>& Cells)
{
	FCellMeshData MeshData;

	const TSet<FIntVector> CellSet(Cells);
	const double HalfSize = Grid.GetCellSize() * 0.5;

	// Первый проход - точный подсчёт наружных граней для Reserve: при
	// миллионах клеток многократные переаллокации массивов вершин дороже,
	// чем лишний прогон дешёвых TSet-проверок.
	int64 ExposedFaceCount = 0;
	for (const FIntVector& Cell : Cells)
	{
		for (const FFaceDef& Face : GFaces)
		{
			if (!CellSet.Contains(Cell + Face.NeighborOffset))
			{
				++ExposedFaceCount;
			}
		}
	}

	MeshData.Vertices.Reserve(ExposedFaceCount * 4);
	MeshData.Normals.Reserve(ExposedFaceCount * 4);
	MeshData.UVs.Reserve(ExposedFaceCount * 4);
	MeshData.Triangles.Reserve(ExposedFaceCount * 6);

	for (const FIntVector& Cell : Cells)
	{
		const FVector Center = Grid.GridToWorld(Cell);

		for (const FFaceDef& Face : GFaces)
		{
			if (CellSet.Contains(Cell + Face.NeighborOffset))
			{
				continue;
			}

			const FVector FaceCenter = Center + Face.Normal * HalfSize;
			const FVector U = Face.U * HalfSize;
			const FVector V = Face.V * HalfSize;

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
