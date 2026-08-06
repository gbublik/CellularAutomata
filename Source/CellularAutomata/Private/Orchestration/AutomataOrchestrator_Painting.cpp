// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Rendering/FilteredCellGridView.h"
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

	// Масштаб считается ТОЙ ЖЕ формулой, что и у настоящих клеток
	// (FInstancedMeshCellGridRenderer::Render()): сначала подгонка меша под шаг
	// решётки - его собственный габарит к размеру клетки, - и только потом
	// множители. Без нормировки призрак живёт в единицах МЕША, а клетки в
	// единицах РЕШЁТКИ, и совпадают они только когда меш случайно размером с
	// клетку; в остальных случаях призрак заметно крупнее или мельче того, что
	// на самом деле появится по нажатию.
	//
	// Нормировка по одной оси (X), а не покомпонентная, - тоже как там: ячейки
	// Вороного решёток за пределами простой кубической неквадратные, и
	// покомпонентная раздавила бы их в куб.
	FVector PreviewScale = FVector::OneVector;
	const double MeshReferenceSize = CellMesh->GetBounds().BoxExtent.X * 2.0;
	if (!FMath::IsNearlyZero(MeshReferenceSize))
	{
		PreviewScale = FVector(Grid->GetLattice().GetPlanarCellSize() / MeshReferenceSize);
	}
	PreviewScale *= CellMeshScaleMultiplier * CellPreviewScaleMultiplier;

	CellPreviewComponent->SetWorldScale3D(PreviewScale);
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
