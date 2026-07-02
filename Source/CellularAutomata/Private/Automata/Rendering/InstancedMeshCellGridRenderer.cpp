#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"

#include "Automata/Grid/CellGrid.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

FInstancedMeshCellGridRenderer::FInstancedMeshCellGridRenderer(UInstancedStaticMeshComponent* InComponent)
	: Component(InComponent)
{
}

void FInstancedMeshCellGridRenderer::SetMesh(UStaticMesh* InMesh)
{
	Mesh = InMesh;
}

void FInstancedMeshCellGridRenderer::SetMaterial(UMaterialInterface* InMaterial)
{
	Material = InMaterial;
}

void FInstancedMeshCellGridRenderer::Render(const FCellGrid& Grid)
{
	UInstancedStaticMeshComponent* Comp = Component.Get();
	if (!Comp)
	{
		UE_LOG(LogTemp, Warning, TEXT("FInstancedMeshCellGridRenderer: component is invalid"));
		return;
	}

	if (Mesh.IsValid())
	{
		Comp->SetStaticMesh(Mesh.Get());
	}
	if (Material.IsValid())
	{
		Comp->SetMaterial(0, Material.Get());
	}

	Comp->ClearInstances();

	// Масштабируем меш так, чтобы его bounding box совпадал с размером клетки -
	// иначе при несовпадении реального размера меша и CellSize сетка получается
	// неровной (щели или наложение соседних кубов).
	FVector InstanceScale = FVector::OneVector;
	if (const UStaticMesh* MeshPtr = Mesh.Get())
	{
		const FVector MeshSize = MeshPtr->GetBounds().BoxExtent * 2.0;
		if (!MeshSize.IsNearlyZero())
		{
			InstanceScale = FVector(Grid.GetCellSize()) / MeshSize;
		}
	}

	TArray<FIntVector> AliveCells;
	Grid.GetAliveCells(AliveCells);

	for (const FIntVector& Cell : AliveCells)
	{
		Comp->AddInstance(FTransform(FQuat::Identity, Grid.GridToWorld(Cell), InstanceScale));
	}
}
