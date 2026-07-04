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
	BeginRender(Grid);
	// Без ограничения на размер чанка - весь PendingTransforms уходит одним
	// вызовом AddInstances(), как и раньше до появления чанкинга; цикл
	// формален (тела достаточно ровно одной итерации), но так BeginRender()/
	// AdvanceRenderChunk() остаются единственным местом с этой логикой.
	while (AdvanceRenderChunk(TNumericLimits<int32>::Max()))
	{
	}
}

void FInstancedMeshCellGridRenderer::BeginRender(const FCellGrid& Grid)
{
	PendingTransforms.Reset();
	PendingCursor = 0;
	LastTimings = FRenderTimings();

	UInstancedStaticMeshComponent* Comp = Component.Get();
	if (!Comp)
	{
		UE_LOG(LogTemp, Warning, TEXT("FInstancedMeshCellGridRenderer: component is invalid"));
		return;
	}

	const double SetMeshStartSeconds = FPlatformTime::Seconds();
	if (Mesh.IsValid())
	{
		Comp->SetStaticMesh(Mesh.Get());
	}
	if (Material.IsValid())
	{
		Comp->SetMaterial(0, Material.Get());
	}
	LastTimings.SetMeshSeconds = FPlatformTime::Seconds() - SetMeshStartSeconds;

	const double ClearStartSeconds = FPlatformTime::Seconds();
	Comp->ClearInstances();
	LastTimings.ClearSeconds = FPlatformTime::Seconds() - ClearStartSeconds;

	// Масштабируем меш так, чтобы его bounding box совпадал с размером клетки -
	// иначе при несовпадении реального размера меша и CellSize сетка получается
	// неровной (щели или наложение соседних кубов).
	const double ScaleStartSeconds = FPlatformTime::Seconds();
	FVector InstanceScale = FVector::OneVector;
	if (const UStaticMesh* MeshPtr = Mesh.Get())
	{
		const FVector MeshSize = MeshPtr->GetBounds().BoxExtent * 2.0;
		if (!MeshSize.IsNearlyZero())
		{
			InstanceScale = FVector(Grid.GetCellSize()) / MeshSize;
		}
	}
	LastTimings.ScaleSeconds = FPlatformTime::Seconds() - ScaleStartSeconds;

	const double GetAliveStartSeconds = FPlatformTime::Seconds();
	TArray<FIntVector> AliveCells;
	Grid.GetAliveCells(AliveCells);
	LastTimings.GetAliveSeconds = FPlatformTime::Seconds() - GetAliveStartSeconds;

	// AddInstance() по одному элементу пересобирает внутренний буфер инстансов
	// на каждый вызов (супралинейный рост при большом числе клеток) - строим
	// все трансформы разом, а добавляем их через AddInstances() в
	// AdvanceRenderChunk() (одним батчем целиком или по частям).
	const double BuildTransformsStartSeconds = FPlatformTime::Seconds();
	PendingTransforms.Reserve(AliveCells.Num());
	for (const FIntVector& Cell : AliveCells)
	{
		PendingTransforms.Add(FTransform(FQuat::Identity, Grid.GridToWorld(Cell), InstanceScale));
	}
	LastTimings.BuildTransformsSeconds = FPlatformTime::Seconds() - BuildTransformsStartSeconds;

	// Логирование намеренно НЕ здесь: UE_LOG сам по себе (форматирование +
	// запись в файл) стоит времени, и если логировать внутри BeginRender(),
	// эта стоимость попадает в измеряемый снаружи интервал (см. Next()/
	// GenerateRandom() в AutomataOrchestrator), но не в одну из полей
	// LastTimings - разница выглядит как необъяснённый пробел в замерах.
	// Вызывающая сторона логирует один раз, объединяя это с шагом симуляции.
}

bool FInstancedMeshCellGridRenderer::AdvanceRenderChunk(int32 MaxCellsThisChunk)
{
	UInstancedStaticMeshComponent* Comp = Component.Get();
	if (!Comp)
	{
		PendingTransforms.Reset();
		PendingCursor = 0;
		return false;
	}

	const int32 Remaining = PendingTransforms.Num() - PendingCursor;
	if (Remaining <= 0)
	{
		return false;
	}

	const int32 CountThisChunk = FMath::Min(MaxCellsThisChunk, Remaining);

	TArray<FTransform> ChunkTransforms;
	ChunkTransforms.Append(PendingTransforms.GetData() + PendingCursor, CountThisChunk);

	// bUpdateNavigation=false: навмеш клеткам автомата не нужен.
	const double AddInstanceStartSeconds = FPlatformTime::Seconds();
	Comp->AddInstances(ChunkTransforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false, /*bUpdateNavigation=*/false);
	LastTimings.AddInstanceSeconds += FPlatformTime::Seconds() - AddInstanceStartSeconds;

	PendingCursor += CountThisChunk;
	return PendingCursor < PendingTransforms.Num();
}
