// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Automata/Editing/CellEditJournal.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

namespace
{
	/** Длина оси манипулятора в ЛОКАЛЬНЫХ единицах - мировую задаёт экранный
	 *  масштаб (см. UpdateSelectionGizmo()). Та же величина, что у гизмо куба,
	 *  и намеренно: два манипулятора в одной сцене, отличающиеся размером,
	 *  читались бы как разные по важности. */
	constexpr float SelectionGizmoAxisLength = 100.0f;

	/** Доля высоты кадра, которую занимает ось. */
	constexpr float SelectionGizmoScreenSize = 0.12f;

	/** Радиус попадания по стрелке, в локальных единицах: заметно больше самой
	 *  стрелки, иначе целиться пришлось бы в пиксель. */
	constexpr float SelectionGizmoPickRadius = 12.0f;

	const FVector GizmoAxisDirections[3] = { FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector };

	/** Меш движка по пути. Именно LoadObject(), а НЕ ConstructorHelpers::
	 *  FObjectFinder: тот работает только внутри конструктора UObject и вне его
	 *  падает фатально (проверка IsInConstructor), а манипулятор создаётся
	 *  лениво, из тика. Ошибка стоила краша редактора на первом же кадре PIE -
	 *  лог обрывался сразу после настройки контроллера, без единого сообщения.
	 *
	 *  Меши из /Engine/BasicShapes есть в любом проекте, отдельного ассета
	 *  заводить не нужно. Единицы: цилиндр и конус 100x100x100 с осью по +Z,
	 *  отсюда все множители ниже. */
	UStaticMesh* LoadEngineShape(const TCHAR* AssetPath)
	{
		return LoadObject<UStaticMesh>(nullptr, AssetPath);
	}
}

void AAutomataOrchestrator::EnsureSelectionGizmoComponents()
{
	if (SelectionGizmoRoot)
	{
		return;
	}

	UStaticMesh* CylinderMesh = LoadEngineShape(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* ConeMesh = LoadEngineShape(TEXT("/Engine/BasicShapes/Cone.Cone"));

	SelectionGizmoRoot = NewObject<USceneComponent>(this, TEXT("SelectionGizmoRoot"));
	SelectionGizmoRoot->SetupAttachment(GetRootComponent());
	SelectionGizmoRoot->RegisterComponent();
	SelectionGizmoRoot->SetVisibility(false);

	SelectionGizmoParts.Reset();
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		// MakeFromZ, а не собранный руками FRotator: меши смотрят вдоль +Z, и
		// так направление читается прямо из кода.
		const FRotator AxisRotation = FRotationMatrix::MakeFromZ(GizmoAxisDirections[Axis]).Rotator();

		UStaticMeshComponent* Shaft = NewObject<UStaticMeshComponent>(this);
		Shaft->SetupAttachment(SelectionGizmoRoot);
		Shaft->RegisterComponent();
		Shaft->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Shaft->SetCastShadow(false);
		if (CylinderMesh)
		{
			Shaft->SetStaticMesh(CylinderMesh);
		}
		Shaft->SetRelativeLocation(GizmoAxisDirections[Axis] * (SelectionGizmoAxisLength * 0.4f));
		Shaft->SetRelativeRotation(AxisRotation);
		Shaft->SetRelativeScale3D(FVector(0.04f, 0.04f, SelectionGizmoAxisLength * 0.008f));
		SelectionGizmoParts.Add(Shaft);

		UStaticMeshComponent* Head = NewObject<UStaticMeshComponent>(this);
		Head->SetupAttachment(SelectionGizmoRoot);
		Head->RegisterComponent();
		Head->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Head->SetCastShadow(false);
		if (ConeMesh)
		{
			Head->SetStaticMesh(ConeMesh);
		}
		Head->SetRelativeLocation(GizmoAxisDirections[Axis] * (SelectionGizmoAxisLength * 0.9f));
		Head->SetRelativeRotation(AxisRotation);
		Head->SetRelativeScale3D(FVector(0.15f, 0.15f, SelectionGizmoAxisLength * 0.002f));
		SelectionGizmoParts.Add(Head);
	}
}

void AAutomataOrchestrator::UpdateSelectionGizmo(const FVector& CameraLocation, float CameraFOVDegrees, bool bVisible)
{
	EnsureSelectionGizmoComponents();
	if (!SelectionGizmoRoot)
	{
		return;
	}

	// Двигать нечего - манипулятор не нужен. Идущий драг при этом не рвём: он
	// закончится своим EndSelectionDrag(), иначе выделение осталось бы
	// нарисованным со сдвигом, которого нет в сетке.
	FVector Center = FVector::ZeroVector;
	float Radius = 0.0f;
	if (!bVisible || !ComputeSelectedCellsBounds(Center, Radius))
	{
		if (!IsSelectionDragging())
		{
			SelectionGizmoRoot->SetVisibility(false, /*bPropagateToChildren=*/true);
		}
		return;
	}

	// Во время драга манипулятор стоит там, где его схватили: центр выделения
	// уезжает вместе с подсветкой, и манипулятор, привязанный к нему, убегал бы
	// из-под курсора.
	const FVector GizmoLocation = IsSelectionDragging() ? SelectionDragOrigin : Center;
	SelectionGizmoRoot->SetWorldLocation(GizmoLocation);

	// Постоянный размер на экране: половина высоты кадра в мире на расстоянии до
	// манипулятора - Distance * tan(FOV/2). Та же формула, что у гизмо куба.
	const float Distance = FVector::Dist(CameraLocation, GizmoLocation);
	const float HalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(CameraFOVDegrees, 1.0f, 170.0f) * 0.5f);
	const float WorldSizeAtDistance = FMath::Max(Distance * FMath::Tan(HalfFovRadians), KINDA_SMALL_NUMBER);

	SelectionGizmoWorldScale = FMath::Max(WorldSizeAtDistance * SelectionGizmoScreenSize / SelectionGizmoAxisLength, KINDA_SMALL_NUMBER);
	SelectionGizmoRoot->SetWorldScale3D(FVector(SelectionGizmoWorldScale));
	SelectionGizmoRoot->SetVisibility(true, /*bPropagateToChildren=*/true);

	// Цвета осей берём У КУБА ОТСЕЧЕНИЯ (AxisMaterialX/Y/Z), а не заводим свои
	// три поля: два манипулятора в одной сцене с разными цветами осей читались
	// бы как разные системы координат. Заодно это единственная настройка,
	// которую пользователь уже заполнил.
	//
	// EnsureRenderCullVolume() здесь безопасен - он только ищет актор, ничего не
	// создавая. Куба в мире может не быть вовсе: тогда стрелки остаются на
	// дефолтном материале меша, то есть одноцветными, и это лучше, чем не
	// показать их вообще.
	if (ARenderCullVolume* CullVolume = EnsureRenderCullVolume())
	{
		UMaterialInterface* AxisMaterials[3] = {
			CullVolume->AxisMaterialX, CullVolume->AxisMaterialY, CullVolume->AxisMaterialZ
		};

		// Материал переназначается на каждом показе, а не один раз при создании:
		// его могли поменять в Details panel, и правка обязана подхватываться без
		// перезапуска - та же конвенция, что у SetMesh/SetMaterial перед каждым
		// Render() у клеток.
		for (int32 Index = 0; Index < SelectionGizmoParts.Num(); ++Index)
		{
			// По две детали на ось (стержень и наконечник) - отсюда деление.
			UMaterialInterface* Material = AxisMaterials[Index / 2];
			if (Material && SelectionGizmoParts[Index])
			{
				SelectionGizmoParts[Index]->SetMaterial(0, Material);
			}
		}
	}
}

bool AAutomataOrchestrator::IsSelectionGizmoVisible() const
{
	return SelectionGizmoRoot && SelectionGizmoRoot->IsVisible();
}

int32 AAutomataOrchestrator::TraceSelectionGizmo(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutAxis) const
{
	OutAxis = FVector::ZeroVector;
	if (!SelectionGizmoRoot || !SelectionGizmoRoot->IsVisible())
	{
		return INDEX_NONE;
	}

	const FVector Origin = SelectionGizmoRoot->GetComponentLocation();
	const FVector Direction = RayDirection.GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		return INDEX_NONE;
	}

	// Мерим ТЕМ ЖЕ масштабом, которым манипулятор нарисован: длина стрелки в
	// мире зависит от расстояния до камеры, и трассировка по локальным
	// величинам заставляла бы целиться мимо видимого.
	const float PickRadius = SelectionGizmoPickRadius * SelectionGizmoWorldScale;
	const float AxisLength = SelectionGizmoAxisLength * SelectionGizmoWorldScale;

	int32 BestAxis = INDEX_NONE;
	float BestDistanceAlongRay = TNumericLimits<float>::Max();

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		// Стрелка - отрезок от центра до кончика; ближайшая точка между ним и
		// лучом. Побеждает та, что ближе к камере: с ракурса, где две стрелки
		// перекрываются, выбор иначе зависел бы от порядка перебора.
		FVector ClosestOnRay = FVector::ZeroVector;
		FVector ClosestOnAxis = FVector::ZeroVector;
		FMath::SegmentDistToSegmentSafe(
			RayOrigin, RayOrigin + Direction * 1.0e7f,
			Origin, Origin + GizmoAxisDirections[Axis] * AxisLength,
			ClosestOnRay, ClosestOnAxis);

		if (FVector::Dist(ClosestOnRay, ClosestOnAxis) > PickRadius)
		{
			continue;
		}

		const float DistanceAlongRay = FVector::Dist(RayOrigin, ClosestOnRay);
		if (DistanceAlongRay < BestDistanceAlongRay)
		{
			BestDistanceAlongRay = DistanceAlongRay;
			BestAxis = Axis;
			OutAxis = GizmoAxisDirections[Axis];
		}
	}

	return BestAxis;
}

void AAutomataOrchestrator::BeginSelectionDrag(int32 Axis, const FVector& AxisDirection, float AxisParam)
{
	if (Axis < 0 || Axis > 2 || SelectedCells.Num() == 0)
	{
		return;
	}

	FVector Center = FVector::ZeroVector;
	float Radius = 0.0f;
	if (!ComputeSelectedCellsBounds(Center, Radius))
	{
		return;
	}

	SelectionDragAxis = Axis;
	SelectionDragAxisDirection = AxisDirection;
	SelectionDragStartParam = AxisParam;
	SelectionDragOrigin = Center;
	SelectionDragCellOffset = FIntVector::ZeroValue;
}

void AAutomataOrchestrator::UpdateSelectionDrag(float AxisParam)
{
	if (!IsSelectionDragging() || !Grid)
	{
		return;
	}

	// Сдвиг вдоль оси в мире - и сразу в клетки: тянуть можно только по
	// решётке, промежуточных положений у клетки не бывает.
	const double WorldDelta = AxisParam - SelectionDragStartParam;
	const FVector CellExtent = Grid->GetLattice().GetCellWorldExtent();
	const double AxisStep = FMath::Max(CellExtent[SelectionDragAxis], UE_DOUBLE_SMALL_NUMBER);

	FIntVector NewOffset = FIntVector::ZeroValue;
	NewOffset[SelectionDragAxis] = FMath::RoundToInt(WorldDelta / AxisStep);

	if (NewOffset == SelectionDragCellOffset)
	{
		return;
	}
	SelectionDragCellOffset = NewOffset;

	// Сетку не трогаем - двигается только подсветка, целиком, одним трансформом
	// (инстансы внутри неё уже на своих местах). Так драг стоит один
	// SetWorldLocation за кадр вместо перестройки выделения, а в журнал уходит
	// одна запись вместо сотни (см. doc-comment UpdateSelectionDrag()).
	if (SelectionMeshComponent)
	{
		const FVector WorldOffset(
			SelectionDragCellOffset.X * CellExtent.X,
			SelectionDragCellOffset.Y * CellExtent.Y,
			SelectionDragCellOffset.Z * CellExtent.Z);
		SelectionMeshComponent->SetWorldLocation(GetActorLocation() + WorldOffset);
	}
}

void AAutomataOrchestrator::EndSelectionDrag()
{
	if (!IsSelectionDragging())
	{
		return;
	}

	const FIntVector Offset = SelectionDragCellOffset;

	SelectionDragAxis = INDEX_NONE;
	SelectionDragAxisDirection = FVector::ZeroVector;
	SelectionDragCellOffset = FIntVector::ZeroValue;

	// Подсветка возвращается на место ВСЕГДА, чем бы драг ни кончился: её
	// смещение было показом, а не состоянием, и оставить его значило бы
	// нарисовать клетки там, где их нет.
	if (SelectionMeshComponent)
	{
		SelectionMeshComponent->SetWorldLocation(GetActorLocation());
	}

	if (Offset == FIntVector::ZeroValue || !Grid)
	{
		return;
	}

	if (bStepInProgress)
	{
		// Фоновый шаг читает *Grid - тот же запрет, что у всех путей правки.
		// Драг при этом уже показал новое положение, поэтому перерисовываем:
		// иначе на экране осталось бы смещённое выделение поверх нетронутой
		// сетки.
		UE_LOG(LogTemp, Warning, TEXT("EndSelectionDrag: фоновый шаг ещё считается - перенос отменён"));
		RenderSelectionOverlay();
		return;
	}

	FCellEditRecord Record = CellEditJournal::MakeMoveRecord(*Grid, SelectedCells, Offset, GenerationCount);
	if (Record.Edits.Num() == 0)
	{
		RenderSelectionOverlay();
		return;
	}

	CellEditJournal::ApplyForward(*Grid, Record);
	RecordEdit(MoveTemp(Record));

	// Выделение едет ВМЕСТЕ с клетками: иначе после переноса оно указывало бы на
	// пустые места, а Delete или ещё один драг работали бы не с тем, что видно.
	for (FIntVector& Cell : SelectedCells)
	{
		Cell += Offset;
	}

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("EndSelectionDrag: выделение перенесено на (%d,%d,%d), живых клеток %d"),
		Offset.X, Offset.Y, Offset.Z, Grid->Num());
}
