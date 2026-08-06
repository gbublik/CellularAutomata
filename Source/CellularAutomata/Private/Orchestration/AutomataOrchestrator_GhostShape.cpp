// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Automata/Meshing/CellMeshBuilder.h"
#include "Automata/Meshing/ChunkGridView.h"
#include "ProceduralMeshComponent.h"


void AAutomataOrchestrator::EnsureGhostMeshComponent()
{
	if (GhostMeshComponent)
	{
		return;
	}

	GhostMeshComponent = NewObject<UProceduralMeshComponent>(this);
	GhostMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostMeshComponent->SetupAttachment(CellsMeshHierarchical);
	GhostMeshComponent->RegisterComponent();
}

void AAutomataOrchestrator::ClearGhostShape()
{
	if (GhostMeshComponent)
	{
		GhostMeshComponent->ClearAllMeshSections();
	}
	GhostShapeGenerationsSinceRefresh = 0;
}

void AAutomataOrchestrator::RefreshGhostShape()
{
	if (!bEnableGhostShape || !Grid)
	{
		ClearGhostShape();
		return;
	}

	TArray<FIntVector> OccupiedChunks;
	Grid->GetOccupiedChunkCoords(OccupiedChunks);
	const FVector ChunkWorldExtent = Grid->GetChunkWorldExtent();
	if (OccupiedChunks.Num() == 0 || ChunkWorldExtent.GetMin() <= 0.0)
	{
		// Нулевой габарит - грид не поддерживает чанкинг (см. doc-comment
		// FCellGrid::GetChunkWorldExtent()) - фича молча ничего не делает.
		ClearGhostShape();
		return;
	}

	// Куб отсечения активен (см. GetActiveCullVolume()) - оставляем только
	// чанки СНАРУЖИ куба, внутри уже рисует обычный детальный путь
	// (BuildCellRenderData()), силуэт здесь чистое дополнение.
	// Иначе (куб выключен хоткеем C либо его вообще нет на уровне) -
	// границы отсечения нет, "снаружи" значит "везде": силуэт покрывает всю
	// сетку целиком и заменяет детальный рендер (см.
	// ShouldGhostShapeReplaceDetailedRender(), RenderGridImmediate()/
	// RenderCurrentGrid()) - именно этот режим и даёт выигрыш в скорости
	// при большом числе клеток без активного куба.
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	TArray<FIntVector> ChunksToGhost;
	if (CullVolume)
	{
		// Чанки на границе (частично внутри/снаружи) сознательно остаются
		// "внутри" (не отбрасываются) - минимальное дублирование на границе
		// дешевле точной обрезки.
		const FBox CullBounds = CullVolume->GetWorldBounds();
		ChunksToGhost.Reserve(OccupiedChunks.Num());
		for (const FIntVector& ChunkCoord : OccupiedChunks)
		{
			const FVector ChunkOrigin = FVector(ChunkCoord) * ChunkWorldExtent;
			const FBox ChunkBounds(ChunkOrigin, ChunkOrigin + ChunkWorldExtent);
			if (!CullBounds.Intersect(ChunkBounds))
			{
				ChunksToGhost.Add(ChunkCoord);
			}
		}
	}
	else
	{
		ChunksToGhost = OccupiedChunks;
	}

	if (ChunksToGhost.Num() == 0)
	{
		ClearGhostShape();
		return;
	}

	UMaterialInterface* MeshMaterial = GhostShapeMaterial;
	if (!MeshMaterial)
	{
		// Фолбэка больше нет: раньше подставлялся AgeMaterials[0], но
		// подставить сюда CellMaterial нельзя - он берёт цвет из per-instance
		// custom data, которых у UProceduralMeshComponent нет, и силуэт вышел
		// бы ЧЁРНЫМ, молча. Серый дефолт движка плюс эта строчка честнее.
		UE_LOG(LogTemp, Warning, TEXT("RefreshGhostShape: GhostShapeMaterial не назначен - силуэт будет нарисован дефолтным материалом движка"));
	}

	const double BuildStartSeconds = FPlatformTime::Seconds();
	FChunkGridView ChunkView(ChunkWorldExtent, Grid->GetLattice().GetCellWorldExtent(), ChunksToGhost);
	CellMeshBuilder::FCellMeshData MeshData = CellMeshBuilder::BuildFromCells(ChunkView, ChunksToGhost);
	const double BuildSeconds = FPlatformTime::Seconds() - BuildStartSeconds;

	EnsureGhostMeshComponent();
	GhostMeshComponent->ClearAllMeshSections();
	GhostMeshComponent->CreateMeshSection_LinearColor(0, MeshData.Vertices, MeshData.Triangles, MeshData.Normals, MeshData.UVs,
		TArray<FLinearColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision=*/false);
	if (MeshMaterial)
	{
		GhostMeshComponent->SetMaterial(0, MeshMaterial);
	}

	UE_LOG(LogTemp, Log, TEXT("RefreshGhostShape: %d/%d чанков %s -> %d вершин / %d треугольников (%.2f мс)"),
		ChunksToGhost.Num(), OccupiedChunks.Num(), CullVolume ? TEXT("снаружи куба") : TEXT("(весь грид, куба нет)"),
		MeshData.Vertices.Num(), MeshData.Triangles.Num() / 3, BuildSeconds * 1000.0);
}

bool AAutomataOrchestrator::ShouldGhostShapeReplaceDetailedRender()
{
	if (!bEnableGhostShape)
	{
		return false;
	}

	// Нет активной границы отсечения (куб выключен либо на уровне его вообще
	// нет - см. GetActiveCullVolume()) - RefreshGhostShape() в
	// этом случае строит силуэт по ВСЕМ занятым чанкам (см. её doc-comment),
	// т.е. он уже покрывает всю сетку целиком, и детальный поклеточный
	// рендер (BuildCellRenderData()+AddInstances по каждой живой клетке) здесь
	// был бы именно той дорогой работой, которую эта фича должна заменять
	// при большом числе клеток. Пока куб активен, детальный путь и так уже
	// дешёвый (Grid->GetAliveCellsInBounds() ограничивает его объёмом куба) -
	// там силуэт остаётся чистым дополнением снаружи, детальный путь не
	// трогаем.
	return GetActiveCullVolume() == nullptr;
}

void AAutomataOrchestrator::SetGhostShapeEnabled(bool bEnabled)
{
	bEnableGhostShape = bEnabled;
	// Тот же флаг "профиль тронули руками", что и в SetCellCullingEnabled().
	// ApplyRenderPreset() зовёт этот сеттер сам и сбрасывает флаг уже после.
	bRenderPresetModified = true;
	UE_LOG(LogTemp, Log, TEXT("SetGhostShapeEnabled: Ghost Shape %s"), bEnabled ? TEXT("включён") : TEXT("выключен"));

	// RefreshGhostShape() сам разберётся, что делать: bEnableGhostShape ==
	// false в его собственном guard'е сведётся к ClearGhostShape() - не
	// нужно дублировать эту ветку здесь. Счётчик сбрасываем всегда, чтобы
	// ручное включение сразу пересчитало силуэт, а не ждало остаток
	// GhostShapeRefreshInterval с прошлого раза.
	GhostShapeGenerationsSinceRefresh = 0;

	if (!Grid)
	{
		// Ещё нет сетки (до первого GenerateRandom()) - перерисовывать
		// нечего, следующий GenerateRandom()/Next() сам учтёт актуальное
		// состояние переключателя (тот же ранний выход, что у
		// RefreshRenderCullVolume() ниже).
		return;
	}

	// Сначала детальный путь - в режиме "куба нет" (см. doc-comment
	// ShouldGhostShapeReplaceDetailedRender()) он теперь сам решит, рисовать
	// всё как обычно, или очиститься и уступить место силуэту целиком; затем
	// сам силуэт. Без этого включение/выключение Ghost Shape хоткеем H
	// визуально ничего не меняло бы до следующего реально посчитанного
	// поколения - тот же принцип, что и у RefreshRenderCullVolume() ниже.
	RenderGridImmediate();
	RefreshGhostShape();
}
