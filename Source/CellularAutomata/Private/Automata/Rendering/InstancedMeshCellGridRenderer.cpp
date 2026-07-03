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

	const double ClearStartSeconds = FPlatformTime::Seconds();
	Comp->ClearInstances();
	const double ClearSeconds = FPlatformTime::Seconds() - ClearStartSeconds;

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

	const double GetAliveStartSeconds = FPlatformTime::Seconds();
	TArray<FIntVector> AliveCells;
	Grid.GetAliveCells(AliveCells);
	const double GetAliveSeconds = FPlatformTime::Seconds() - GetAliveStartSeconds;

	// AddInstance() по одному элементу пересобирает внутренний буфер инстансов
	// на каждый вызов (супралинейный рост при большом числе клеток) - строим
	// все трансформы разом и добавляем их одним батчем через AddInstances().
	// bUpdateNavigation=false: навмеш клеткам автомата не нужен.
	TArray<FTransform> InstanceTransforms;
	InstanceTransforms.Reserve(AliveCells.Num());
	for (const FIntVector& Cell : AliveCells)
	{
		InstanceTransforms.Add(FTransform(FQuat::Identity, Grid.GridToWorld(Cell), InstanceScale));
	}

	const double AddInstanceStartSeconds = FPlatformTime::Seconds();
	Comp->AddInstances(InstanceTransforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false, /*bUpdateNavigation=*/false);
	const double AddInstanceSeconds = FPlatformTime::Seconds() - AddInstanceStartSeconds;

	UE_LOG(LogTemp, Log, TEXT("FInstancedMeshCellGridRenderer::Render: %d клеток (ClearInstances: %.2f мс, GetAliveCells: %.2f мс, AddInstances-батч: %.2f мс)"),
		AliveCells.Num(), ClearSeconds * 1000.0, GetAliveSeconds * 1000.0, AddInstanceSeconds * 1000.0);
}
