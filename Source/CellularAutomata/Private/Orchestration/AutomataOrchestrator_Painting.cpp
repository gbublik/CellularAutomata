// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Automata/Editing/CellClipboard.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Rendering/FilteredCellGridView.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Automata/Selection/CellSelection.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"


void AAutomataOrchestrator::EnsureCellPreviewComponent()
{
	if (CellPreviewComponent)
	{
		return;
	}

	CellPreviewComponent = NewObject<UStaticMeshComponent>(this);
	CellPreviewComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Призрак не должен ни отбрасывать тень, ни попадать в фотографию как
	// часть структуры: он - курсор, а не клетка.
	CellPreviewComponent->SetCastShadow(false);
	CellPreviewComponent->SetupAttachment(CellsMeshHierarchical);
	CellPreviewComponent->RegisterComponent();
	CellPreviewComponent->SetVisibility(false);
}

bool AAutomataOrchestrator::ComputePlacementCell(const FVector& RayOrigin, const FVector& RayDirection, FIntVector& OutCell)
{
	FIntVector UnusedNormal;
	return ComputePlacementTarget(RayOrigin, RayDirection, OutCell, UnusedNormal);
}

FVector AAutomataOrchestrator::ComputeCellMeshScale(float ExtraMultiplier) const
{
	// Та же формула, что в FInstancedMeshCellGridRenderer::Render(): сначала
	// подгонка собственного габарита меша под шаг решётки, и только потом
	// множители. Без первой части призрак живёт в единицах МЕША, а клетки в
	// единицах РЕШЁТКИ, и совпадают они только когда меш случайно размером с
	// клетку. Нормировка по одной оси (X), а не покомпонентная: ячейки Вороного
	// решёток за пределами простой кубической неквадратные, покомпонентная
	// раздавила бы их в куб.
	FVector Scale = FVector::OneVector;
	if (Grid && CellMesh)
	{
		const double MeshReferenceSize = CellMesh->GetBounds().BoxExtent.X * 2.0;
		if (!FMath::IsNearlyZero(MeshReferenceSize))
		{
			Scale = FVector(Grid->GetLattice().GetPlanarCellSize() / MeshReferenceSize);
		}
	}
	return Scale * (CellMeshScaleMultiplier * ExtraMultiplier);
}

bool AAutomataOrchestrator::ComputePlacementTarget(const FVector& RayOrigin, const FVector& RayDirection,
												   FIntVector& OutCell, FIntVector& OutFaceNormal)
{
	OutFaceNormal = FIntVector::ZeroValue;

	if (!Grid)
	{
		return false;
	}

	const FVector Direction = RayDirection.GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		return false;
	}

	// Тот же лимит обхода и та же фильтрация кубом отсечения, что у
	// SelectCellUnderCursor(): клик обязан видеть ровно то, что нарисовано, -
	// иначе клетка прилипнет к грани, которой на экране нет.
	FVector BoundsCenter = FVector::ZeroVector;
	float BoundsRadius = 0.0f;
	const bool bHaveCells = ComputeAliveCellsBounds(BoundsCenter, BoundsRadius);

	if (bHaveCells)
	{
		const double MaxDistance = FVector::Distance(RayOrigin, BoundsCenter) + BoundsRadius + Grid->GetLattice().GetMaxCellWorldExtent();

		ARenderCullVolume* CullVolume = GetActiveCullVolume();
		TUniquePtr<FFilteredCellGridView> VisibleView;
		const FCellGrid* PickGrid = Grid.Get();
		if (CullVolume)
		{
			TArray<FIntVector> VisibleCells;
			Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), VisibleCells);
			VisibleView = MakeUnique<FFilteredCellGridView>(*Grid, MoveTemp(VisibleCells));
			PickGrid = VisibleView.Get();
		}

		FIntVector HitCell;
		FIntVector FaceNormal;
		if (CellSelection::PickCellAlongRay(*PickGrid, RayOrigin, Direction, MaxDistance, HitCell, FaceNormal))
		{
			// Нулевая нормаль - камера внутри самой клетки, грани входа нет (см.
			// PickCellAlongRay()). Прилипать не к чему, поэтому падаем в ветку
			// "поставить на расстоянии" ниже, а не ставим клетку саму в себя.
			if (FaceNormal != FIntVector::ZeroValue)
			{
				OutCell = HitCell + FaceNormal;
				OutFaceNormal = FaceNormal;
				return true;
			}
		}
	}

	// Промах - штатный случай, а не ошибка: так начинается новая фигура в
	// пустоте. Клетка садится на решётку там, где луч прошёл CellPlaceDistance.
	const FVector PlacePoint = RayOrigin + Direction * CellPlaceDistance;
	OutCell = Grid->GetLattice().WorldToGrid(PlacePoint);
	return true;
}

void AAutomataOrchestrator::UpdateCellPreview(const FVector& RayOrigin, const FVector& RayDirection)
{
	EnsureCellPreviewComponent();
	if (!CellPreviewComponent)
	{
		return;
	}

	FIntVector TargetCell;
	if (!Grid || !CellMesh || !ComputePlacementCell(RayOrigin, RayDirection, TargetCell))
	{
		HideCellPreview();
		return;
	}

	// Меш и материал переставляем только при смене - SetStaticMesh() пересоздаёт
	// render state, а зовётся это каждый кадр.
	if (CellPreviewComponent->GetStaticMesh() != CellMesh)
	{
		CellPreviewComponent->SetStaticMesh(CellMesh);
		CellPreviewComponent->SetMaterial(0, EnsureCellMaterialInstance());
	}

	const FVector WorldLocation = Grid->GetLattice().GridToWorld(TargetCell);
	CellPreviewComponent->SetWorldLocation(WorldLocation);

	CellPreviewComponent->SetWorldScale3D(ComputeCellMeshScale(CellPreviewScaleMultiplier));
	CellPreviewComponent->SetVisibility(true);
}

void AAutomataOrchestrator::HideCellPreview()
{
	if (CellPreviewComponent)
	{
		CellPreviewComponent->SetVisibility(false);
	}
}

void AAutomataOrchestrator::PaintCellUnderCursor(const FVector& RayOrigin, const FVector& RayDirection, bool bErase)
{
	if (!Grid)
	{
		return;
	}

	// Мутировать Grid под идущим StepAsync() нельзя - тот же запрет, что и у
	// всех остальных путей правки. Клик редок (одна клетка на нажатие), так
	// что отказ можно и озвучить: молчание здесь читалось бы как "мышь не
	// сработала".
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("PaintCellUnderCursor: фоновый шаг ещё считается - клетка не поставлена"));
		return;
	}

	FIntVector TargetCell;
	if (bErase)
	{
		// Стирание бьёт по САМОЙ клетке под курсором, а не по соседней с ней:
		// постановка липнет к грани снаружи, удаление снимает то, на что
		// показывают. Одна и та же грань, разные клетки - и это ровно то, чего
		// ждёшь от пары "поставить/убрать" в воксельном редакторе.
		FVector BoundsCenter = FVector::ZeroVector;
		float BoundsRadius = 0.0f;
		if (!ComputeAliveCellsBounds(BoundsCenter, BoundsRadius))
		{
			return;
		}

		const FVector Direction = RayDirection.GetSafeNormal();
		const double MaxDistance = FVector::Distance(RayOrigin, BoundsCenter) + BoundsRadius + Grid->GetLattice().GetMaxCellWorldExtent();
		if (!CellSelection::PickCellAlongRay(*Grid, RayOrigin, Direction, MaxDistance, TargetCell))
		{
			return; // мимо всего живого - стирать нечего
		}
	}
	else if (!ComputePlacementCell(RayOrigin, RayDirection, TargetCell))
	{
		return;
	}

	// Клетка, уже находящаяся в нужном состоянии, отсеивается самой записью
	// (MakeAddRecord/MakeDeleteRecord пропускают такие): пустая запись значит
	// "ничего не изменилось", и в журнал ей нельзя - Ctrl+Z после такого клика
	// отменял бы "ничего" и выглядел бы несработавшим.
	const TArray<FIntVector> One = { TargetCell };
	FCellEditRecord Edit = bErase
		? CellEditJournal::MakeDeleteRecord(*Grid, One, GenerationCount)
		: CellEditJournal::MakeAddRecord(*Grid, One, GenerationCount);

	if (Edit.Edits.Num() == 0)
	{
		return;
	}

	CellEditJournal::ApplyForward(*Grid, Edit);

	Edit.Description = bErase
		? FString::Printf(TEXT("убрана клетка (%d,%d,%d)"), TargetCell.X, TargetCell.Y, TargetCell.Z)
		: FString::Printf(TEXT("поставлена клетка (%d,%d,%d)"), TargetCell.X, TargetCell.Y, TargetCell.Z);
	RecordEdit(MoveTemp(Edit));

	// Немедленно и целиком: правка рукой - осознанное одиночное действие, и
	// размазывать её по кадрам чанковым рендером незачем (та же причина, что у
	// DeleteSelectedCells()).
	RenderGridImmediate();
}

void AAutomataOrchestrator::EnsureClipboardGhostComponent()
{
	if (ClipboardGhostComponent)
	{
		return;
	}

	// Обычный ISM, а не HISM: буфер - это кусок, вырезанный руками, LOD-дерево
	// кластеров на нём не окупается (та же причина, что у
	// SelectionMeshComponent).
	ClipboardGhostComponent = NewObject<UInstancedStaticMeshComponent>(this);
	ClipboardGhostComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ClipboardGhostComponent->SetCastShadow(false);
	ClipboardGhostComponent->SetupAttachment(CellsMeshHierarchical);
	ClipboardGhostComponent->RegisterComponent();
	ClipboardGhostComponent->SetVisibility(false);
}

void AAutomataOrchestrator::RebuildClipboardGhostInstances()
{
	EnsureClipboardGhostComponent();
	if (!ClipboardGhostComponent || !Grid)
	{
		return;
	}

	ClipboardGhostComponent->ClearInstances();
	if (ClipboardCells.Num() == 0 || !CellMesh)
	{
		return;
	}

	ClipboardGhostComponent->SetStaticMesh(CellMesh);
	ClipboardGhostComponent->SetMaterial(0, EnsureCellMaterialInstance());

	// Инстансы кладутся в ЛОКАЛЬНЫХ координатах компонента, а буфер нормализован
	// вокруг нуля - поэтому вставать на место под курсором будет сам компонент,
	// одним SetWorldLocation() за кадр, а этот цикл больше не повторится (см.
	// UpdateClipboardGhost()).
	const FVector InstanceScale = ComputeCellMeshScale(CellPreviewScaleMultiplier);
	const FLatticeTransform& Lattice = Grid->GetLattice();

	TArray<FTransform> Transforms;
	Transforms.Reserve(ClipboardCells.Num());
	for (const FIntVector& Cell : ClipboardCells)
	{
		// GridToWorld() ОТ НУЛЯ решётки: это смещение клетки внутри буфера, а не
		// её мировая позиция - мировую даёт трансформ компонента.
		Transforms.Emplace(FQuat::Identity, Lattice.GridToWorld(Cell), InstanceScale);
	}
	ClipboardGhostComponent->AddInstances(Transforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
}

void AAutomataOrchestrator::CopyCellsToClipboard()
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("CopyCellsToClipboard: сетка не инициализирована"));
		return;
	}

	// Выделение, если оно есть, иначе вся сетка - тот же принцип, что у
	// ArrayCells(). Из выделения берём только живые: оно переживает шаги, и
	// клетка под ним могла умереть.
	TArray<FIntVector> Source;
	const bool bFromSelection = SelectedCells.Num() > 0;
	if (bFromSelection)
	{
		Source.Reserve(SelectedCells.Num());
		for (const FIntVector& Cell : SelectedCells)
		{
			if (Grid->IsAlive(Cell))
			{
				Source.Add(Cell);
			}
		}
	}
	else
	{
		Grid->GetAliveCells(Source);
	}

	if (Source.Num() == 0)
	{
		const FString Message = bFromSelection
			? TEXT("Копирование: в выделении нет живых клеток")
			: TEXT("Копирование: сетка пуста");
		UE_LOG(LogTemp, Warning, TEXT("CopyCellsToClipboard: %s"), *Message);
		ShowStatusMessage(StatusKey_Clipboard, Message);
		return;
	}

	// Нормализация делает буфер независимым от того места, где кусок вырезали:
	// вставка задаёт положение точкой в пространстве, а не сдвигом.
	CellClipboard::Normalize(Source);
	ClipboardCells = MoveTemp(Source);

	// Инстансы призрака - один раз здесь, а не каждый кадр (см. doc-comment
	// UpdateClipboardGhost()).
	RebuildClipboardGhostInstances();

	UE_LOG(LogTemp, Log, TEXT("CopyCellsToClipboard: скопировано %d клеток (%s)"),
		ClipboardCells.Num(), bFromSelection ? TEXT("выделение") : TEXT("вся сетка"));

	ShowStatusMessage(StatusKey_Clipboard,
		FString::Printf(TEXT("Скопировано клеток: %d - Ctrl+V вставит под курсор"), ClipboardCells.Num()));
}

void AAutomataOrchestrator::UpdateClipboardGhost(const FVector& RayOrigin, const FVector& RayDirection)
{
	if (ClipboardCells.Num() == 0)
	{
		HideClipboardGhost();
		return;
	}

	EnsureClipboardGhostComponent();
	if (!ClipboardGhostComponent || !Grid)
	{
		return;
	}

	FIntVector BaseCell;
	FIntVector FaceNormal;
	if (!ComputePlacementTarget(RayOrigin, RayDirection, BaseCell, FaceNormal))
	{
		HideClipboardGhost();
		return;
	}

	FIntVector BufferMin, BufferMax;
	CellClipboard::ComputeBounds(ClipboardCells, BufferMin, BufferMax);
	const FIntVector Origin = CellClipboard::ComputePasteOrigin(BufferMin, BufferMax, BaseCell, FaceNormal);

	// Весь предпросмотр - одно перемещение компонента: инстансы внутри него
	// стоят на своих местах с момента копирования.
	ClipboardGhostComponent->SetWorldLocation(Grid->GetLattice().GridToWorld(Origin));
	ClipboardGhostComponent->SetVisibility(true);
}

void AAutomataOrchestrator::HideClipboardGhost()
{
	if (ClipboardGhostComponent)
	{
		ClipboardGhostComponent->SetVisibility(false);
	}
}

void AAutomataOrchestrator::PasteClipboard(const FVector& RayOrigin, const FVector& RayDirection)
{
	if (ClipboardCells.Num() == 0)
	{
		ShowStatusMessage(StatusKey_Clipboard, TEXT("Буфер пуст - сначала скопируйте (Ctrl+Shift+C)"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("PasteClipboard: сетка не инициализирована"));
		return;
	}

	// Мутируем Grid - тот же запрет, что у всех путей правки.
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("PasteClipboard: фоновый шаг ещё считается - вставка не выполнена"));
		return;
	}

	FIntVector BaseCell;
	FIntVector FaceNormal;
	if (!ComputePlacementTarget(RayOrigin, RayDirection, BaseCell, FaceNormal))
	{
		return;
	}

	FIntVector BufferMin, BufferMax;
	CellClipboard::ComputeBounds(ClipboardCells, BufferMin, BufferMax);
	const FIntVector Origin = CellClipboard::ComputePasteOrigin(BufferMin, BufferMax, BaseCell, FaceNormal);

	TArray<FIntVector> Placed;
	CellClipboard::Place(ClipboardCells, Origin, Placed);

	// ОДНА запись на всю вставку: в отличие от рисования по клетке, вставка и по
	// смыслу одно действие. Уже живые клетки запись отсеет сама - вставка
	// ДОБАВЛЯЕТ, а не заменяет область, так что накладка на существующую
	// структуру ничего не стирает.
	FCellEditRecord Record = CellEditJournal::MakeAddRecord(*Grid, Placed, GenerationCount);
	const int32 AddedCount = Record.Edits.Num();
	if (AddedCount == 0)
	{
		ShowStatusMessage(StatusKey_Clipboard, TEXT("Вставка: все клетки буфера здесь уже живые"));
		return;
	}

	Record.Description = FString::Printf(TEXT("вставлено клеток: %d"), AddedCount);
	CellEditJournal::ApplyForward(*Grid, Record);
	RecordEdit(MoveTemp(Record));

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("PasteClipboard: вставлено %d из %d клеток буфера в (%d,%d,%d), живых стало %d"),
		AddedCount, ClipboardCells.Num(), Origin.X, Origin.Y, Origin.Z, Grid->Num());

	ShowStatusMessage(StatusKey_Clipboard,
		FString::Printf(TEXT("Вставлено клеток: %d"), AddedCount));
}
