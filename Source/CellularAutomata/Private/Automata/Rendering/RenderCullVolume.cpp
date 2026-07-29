#include "Automata/Rendering/RenderCullVolume.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

#include "Orchestration/AutomataOrchestrator.h"
#include "Kismet/GameplayStatics.h"

ARenderCullVolume::ARenderCullVolume()
{
	PrimaryActorTick.bCanEverTick = false;

	BoundsBox = CreateDefaultSubobject<UBoxComponent>(TEXT("BoundsBox"));
	SetRootComponent(BoundsBox);

	BoundsBox->SetBoxExtent(FVector(50.0f, 50.0f, 50.0f));
	BoundsBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BoundsBox->SetGenerateOverlapEvents(false);
	// Видим и в PIE, не только в редакторе - удобно подгонять границы куба
	// по живой картинке, не только по вьюпорту редактора.
	BoundsBox->SetHiddenInGame(false);

	// Залитый куб под VolumeMaterial - см. doc-comment VolumeMesh. Создаётся
	// всегда, но остаётся скрытым, пока материал не назначен: непрозрачный
	// дефолтный материал закрыл бы собой ровно те клетки, ради которых
	// отсечение и включают.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> VolumeCubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));

	VolumeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VolumeMesh"));
	VolumeMesh->SetupAttachment(BoundsBox);
	if (VolumeCubeMesh.Succeeded())
	{
		VolumeMesh->SetStaticMesh(VolumeCubeMesh.Object);
	}
	// Та же конвенция, что у клеток и у ручек манипулятора: чисто визуальная
	// геометрия, без коллизии и без участия в освещении.
	VolumeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VolumeMesh->SetGenerateOverlapEvents(false);
	VolumeMesh->SetCastShadow(false);
	VolumeMesh->SetVisibility(false);
	VolumeMesh->SetHiddenInGame(true);

	BuildGizmoComponents();
}

void ARenderCullVolume::BeginPlay()
{
	Super::BeginPlay();

	ApplyVolumeVisuals();
}

void ARenderCullVolume::SetVolumeVisible(bool bVisible)
{
	if (bVolumeVisible == bVisible)
	{
		return;
	}
	bVolumeVisible = bVisible;

	ApplyVolumeVisuals();

	// Видимость теперь влияет и на само отсечение (спрятанный куб не режет -
	// см. AAutomataOrchestrator::GetActiveCullVolume()), так что переключение
	// обязано перерисовать кадр: иначе отрезанные клетки вернулись бы на экран
	// только со следующим поколением, а на паузе - вообще никогда. Та же
	// причина, по которой перерисовку зовёт EndGizmoDrag().
	NotifyOrchestratorToRefresh();
}

void ARenderCullVolume::ApplyVolumeVisuals()
{
	if (BoundsBox)
	{
		BoundsBox->SetHiddenInGame(!bVolumeVisible);
	}

	if (!VolumeMesh)
	{
		return;
	}

	// Материал переназначаем на каждом вызове, а не один раз в конструкторе -
	// его могли поменять в Details panel, и правка должна подхватываться без
	// перезапуска (та же конвенция, что у осей манипулятора в
	// SetGizmoVisible() и у SetMesh/SetMaterial перед каждым Render()).
	// Сравнение с уже назначенным - не микрооптимизация: этот метод зовётся и
	// каждый кадр драга ручки масштаба (см. UpdateGizmoDrag()), а SetMaterial()
	// помечает render state компонента грязным даже при том же материале.
	if (VolumeMaterial && VolumeMesh->GetMaterial(0) != VolumeMaterial)
	{
		VolumeMesh->SetMaterial(0, VolumeMaterial);
	}

	// Меш - куб 100x100x100 с центром в нуле, BoxExtent - полуразмер без учёта
	// масштаба актёра (его VolumeMesh наследует от BoundsBox сам), отсюда /50.
	const FVector Extent = BoundsBox ? BoundsBox->GetUnscaledBoxExtent() : FVector(50.0f);
	VolumeMesh->SetRelativeScale3D(Extent / 50.0f);

	const bool bShowMesh = bVolumeVisible && VolumeMaterial != nullptr;
	VolumeMesh->SetVisibility(bShowMesh);
	VolumeMesh->SetHiddenInGame(!bShowMesh);
}

void ARenderCullVolume::BuildGizmoComponents()
{
	// Меши из /Engine/BasicShapes - есть в любом проекте. Единицы: и цилиндр,
	// и конус, и куб - 100x100x100 с центром в начале координат, ось цилиндра
	// и конуса направлена по +Z. Отсюда все множители ниже.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));

	GizmoRoot = CreateDefaultSubobject<USceneComponent>(TEXT("GizmoRoot"));
	GizmoRoot->SetupAttachment(BoundsBox);
	// Манипулятор не должен наследовать масштаб куба (его масштабируют
	// десятками) - экранный масштаб выставляется через SetWorldScale3D в
	// UpdateGizmoScreenSize(), а абсолютность здесь избавляет от деления на
	// родительский масштаб.
	GizmoRoot->SetUsingAbsoluteScale(true);

	const TCHAR* AxisNames[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
	const FVector AxisDirections[3] = { FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector };

	TranslateShafts.SetNum(3);
	TranslateHeads.SetNum(3);
	ScaleHandles.SetNum(3);

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		// MakeFromZ, а не собранный вручную FRotator: меши смотрят вдоль +Z, и
		// так направление читается прямо из кода, без гадания про порядок
		// pitch/yaw/roll.
		const FRotator AxisRotation = FRotationMatrix::MakeFromZ(AxisDirections[Axis]).Rotator();

		UStaticMeshComponent* Shaft = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("GizmoShaft%s"), AxisNames[Axis]));
		Shaft->SetupAttachment(GizmoRoot);
		if (CylinderMesh.Succeeded())
		{
			Shaft->SetStaticMesh(CylinderMesh.Object);
		}
		// Стержень: тонкий цилиндр от центра до 80% длины оси.
		Shaft->SetRelativeLocation(AxisDirections[Axis] * (GizmoAxisLength * 0.4f));
		Shaft->SetRelativeRotation(AxisRotation);
		Shaft->SetRelativeScale3D(FVector(0.04f, 0.04f, GizmoAxisLength * 0.008f));
		TranslateShafts[Axis] = Shaft;

		UStaticMeshComponent* Head = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("GizmoHead%s"), AxisNames[Axis]));
		Head->SetupAttachment(GizmoRoot);
		if (ConeMesh.Succeeded())
		{
			Head->SetStaticMesh(ConeMesh.Object);
		}
		// Наконечник: последние 20% оси.
		Head->SetRelativeLocation(AxisDirections[Axis] * (GizmoAxisLength * 0.9f));
		Head->SetRelativeRotation(AxisRotation);
		Head->SetRelativeScale3D(FVector(0.15f, 0.15f, GizmoAxisLength * 0.002f));
		TranslateHeads[Axis] = Head;

		UStaticMeshComponent* ScaleHandle = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("GizmoScale%s"), AxisNames[Axis]));
		ScaleHandle->SetupAttachment(GizmoRoot);
		if (CubeMesh.Succeeded())
		{
			ScaleHandle->SetStaticMesh(CubeMesh.Object);
		}
		// Кубик масштаба - дальше стрелки по той же оси, чтобы попадания не
		// перекрывались (см. TraceGizmoHandle(): он проверяет кубики первыми).
		ScaleHandle->SetRelativeLocation(AxisDirections[Axis] * (GizmoAxisLength * 1.25f));
		ScaleHandle->SetRelativeScale3D(FVector(0.12f));
		ScaleHandles[Axis] = ScaleHandle;
	}

	// Коллизия ручкам не нужна: попадание считается вручную в
	// TraceGizmoHandle() (та же конвенция, что у клеток и у самого BoundsBox).
	// Скрыты до тех пор, пока контроллер не включит режим взаимодействия.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		for (UStaticMeshComponent* Component : { TranslateShafts[Axis].Get(), TranslateHeads[Axis].Get(), ScaleHandles[Axis].Get() })
		{
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetGenerateOverlapEvents(false);
			Component->SetCastShadow(false);
			Component->SetVisibility(false);
			Component->SetHiddenInGame(true);
		}
	}
}

void ARenderCullVolume::SetGizmoVisible(bool bVisible)
{
	if (bGizmoVisible == bVisible)
	{
		return;
	}
	bGizmoVisible = bVisible;

	UMaterialInterface* AxisMaterials[3] = { AxisMaterialX, AxisMaterialY, AxisMaterialZ };

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		for (UStaticMeshComponent* Component : { TranslateShafts[Axis].Get(), TranslateHeads[Axis].Get(), ScaleHandles[Axis].Get() })
		{
			if (!Component)
			{
				continue;
			}

			// Материал переназначаем на каждом показе, а не один раз в
			// конструкторе - его могли поменять в Details panel, и правка
			// должна подхватываться без перезапуска (та же конвенция, что у
			// SetMesh/SetMaterial перед каждым Render() у клеток).
			if (AxisMaterials[Axis])
			{
				Component->SetMaterial(0, AxisMaterials[Axis]);
			}

			Component->SetVisibility(bVisible);
			Component->SetHiddenInGame(!bVisible);
		}
	}

	if (bVisible && (!AxisMaterialX || !AxisMaterialY || !AxisMaterialZ))
	{
		UE_LOG(LogTemp, Warning, TEXT("ARenderCullVolume: не назначены материалы осей манипулятора - ручки видны, но все одного цвета (назначьте AxisMaterialX/Y/Z в Details panel)"));
	}
}

void ARenderCullVolume::UpdateGizmoScreenSize(const FVector& CameraLocation, float CameraFOVDegrees)
{
	if (!bGizmoVisible || !GizmoRoot)
	{
		return;
	}

	// Половина высоты кадра в мировых единицах на расстоянии до манипулятора:
	// Distance * tan(FOV/2). Умноженная на GizmoScreenSize, это та мировая
	// длина, которую должна занимать ось - делим на её локальную длину и
	// получаем масштаб. FOV в UE горизонтальный, для ощущения "как в
	// редакторе" этого приближения достаточно.
	const float Distance = FVector::Dist(CameraLocation, GetActorLocation());
	const float HalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(CameraFOVDegrees, 1.0f, 170.0f) * 0.5f);
	const float WorldSizeAtDistance = FMath::Max(Distance * FMath::Tan(HalfFovRadians), KINDA_SMALL_NUMBER);

	GizmoWorldScale = FMath::Max(WorldSizeAtDistance * GizmoScreenSize / GizmoAxisLength, KINDA_SMALL_NUMBER);
	GizmoRoot->SetWorldScale3D(FVector(GizmoWorldScale));
}

EVolumeGizmoHandle ARenderCullVolume::TraceGizmoHandle(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutAxis) const
{
	OutAxis = FVector::ZeroVector;
	if (!bGizmoVisible)
	{
		return EVolumeGizmoHandle::None;
	}

	const FVector Origin = GetActorLocation();
	const FVector Direction = RayDirection.GetSafeNormal();
	const float PickRadius = GizmoPickRadius * GizmoWorldScale;
	const float AxisLength = GizmoAxisLength * GizmoWorldScale;

	const FVector AxisDirections[3] = { FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector };

	EVolumeGizmoHandle BestHandle = EVolumeGizmoHandle::None;
	float BestDistanceAlongRay = TNumericLimits<float>::Max();

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		// Кубик масштаба проверяем как сферу вокруг его центра, стержень - как
		// отрезок от центра до кончика стрелки. Оба через ближайшую точку
		// между лучом и отрезком/точкой; побеждает то, что ближе к камере,
		// чтобы перекрывающиеся с ракурса ручки вели себя предсказуемо.
		const FVector ScaleHandleCenter = Origin + AxisDirections[Axis] * (AxisLength * 1.25f);
		const FVector ToHandle = ScaleHandleCenter - RayOrigin;
		const float ProjectionOnRay = FVector::DotProduct(ToHandle, Direction);
		if (ProjectionOnRay > 0.0f)
		{
			const float DistanceToAxisPoint = FVector::Dist(RayOrigin + Direction * ProjectionOnRay, ScaleHandleCenter);
			if (DistanceToAxisPoint <= PickRadius && ProjectionOnRay < BestDistanceAlongRay)
			{
				BestDistanceAlongRay = ProjectionOnRay;
				BestHandle = (EVolumeGizmoHandle)((uint8)EVolumeGizmoHandle::ScaleX + Axis);
				OutAxis = AxisDirections[Axis];
			}
		}

		FVector ClosestOnRay, ClosestOnAxis;
		FMath::SegmentDistToSegmentSafe(
			RayOrigin, RayOrigin + Direction * 1.0e7f,
			Origin, Origin + AxisDirections[Axis] * AxisLength,
			ClosestOnRay, ClosestOnAxis);

		if (FVector::Dist(ClosestOnRay, ClosestOnAxis) <= PickRadius)
		{
			const float DistanceAlongRay = FVector::DotProduct(ClosestOnRay - RayOrigin, Direction);
			if (DistanceAlongRay > 0.0f && DistanceAlongRay < BestDistanceAlongRay)
			{
				BestDistanceAlongRay = DistanceAlongRay;
				BestHandle = (EVolumeGizmoHandle)((uint8)EVolumeGizmoHandle::TranslateX + Axis);
				OutAxis = AxisDirections[Axis];
			}
		}
	}

	return BestHandle;
}

void ARenderCullVolume::BeginGizmoDrag(EVolumeGizmoHandle Handle, const FVector& Axis, float AxisParam)
{
	ActiveGizmoHandle = Handle;
	ActiveGizmoAxis = Axis.GetSafeNormal();
	DragStartAxisParam = AxisParam;
	DragStartActorLocation = GetActorLocation();
	DragStartBoxExtent = BoundsBox->GetUnscaledBoxExtent();
}

void ARenderCullVolume::UpdateGizmoDrag(float AxisParam, bool bUniformScale)
{
	if (ActiveGizmoHandle == EVolumeGizmoHandle::None)
	{
		return;
	}

	const float Delta = AxisParam - DragStartAxisParam;

	switch (ActiveGizmoHandle)
	{
	case EVolumeGizmoHandle::TranslateX:
	case EVolumeGizmoHandle::TranslateY:
	case EVolumeGizmoHandle::TranslateZ:
		SetActorLocation(DragStartActorLocation + ActiveGizmoAxis * Delta);
		break;

	case EVolumeGizmoHandle::ScaleX:
	case EVolumeGizmoHandle::ScaleY:
	case EVolumeGizmoHandle::ScaleZ:
	{
		// Ручка тянет ОДНУ грань, но куб остаётся симметричным относительно
		// своего центра (GetWorldBounds() строит AABB как Location +- Extent),
		// поэтому смещение ручки на Delta - это изменение полуразмера на
		// столько же. Delta в мире, а BoxExtent хранится без учёта масштаба
		// актёра - делим.
		const int32 AxisIndex = (int32)ActiveGizmoHandle - (int32)EVolumeGizmoHandle::ScaleX;
		const FVector ActorScale = GetActorScale3D();
		const float ScaleOnAxis = FMath::Abs(ActorScale[AxisIndex]) > KINDA_SMALL_NUMBER ? ActorScale[AxisIndex] : 1.0f;

		FVector NewExtent = DragStartBoxExtent;
		// Нижняя граница, а не просто >0: вывернутый наизнанку или нулевой
		// куб отсекал бы вообще всё, и вернуть его мышью было бы уже нечем.
		NewExtent[AxisIndex] = FMath::Max(DragStartBoxExtent[AxisIndex] + Delta / ScaleOnAxis, 1.0f);

		if (bUniformScale)
		{
			// Соразмерно - значит по ОТНОШЕНИЮ, а не прибавляя ту же Delta ко
			// всем осям: одинаковая добавка к разным полуразмерам гонит
			// коробку к кубу, теряя заданную пропорцию, а нужно сохранить её.
			// Отношение берётся от значений НА НАЧАЛО драга, а не от текущих,
			// иначе оно накапливалось бы кадр за кадром и коробка улетала бы
			// экспоненциально.
			const float StartOnAxis = DragStartBoxExtent[AxisIndex];
			if (StartOnAxis > KINDA_SMALL_NUMBER)
			{
				const float Ratio = NewExtent[AxisIndex] / StartOnAxis;
				for (int32 Index = 0; Index < 3; ++Index)
				{
					if (Index != AxisIndex)
					{
						NewExtent[Index] = FMath::Max(DragStartBoxExtent[Index] * Ratio, 1.0f);
					}
				}
			}
		}

		BoundsBox->SetBoxExtent(NewExtent);
		// Залитый куб тянется за проволочным кадр в кадр - иначе его масштаб
		// остался бы от начала драга и разошёлся бы с границами, по которым
		// реально режутся клетки (перерисовка клеток по-прежнему только на
		// EndGizmoDrag(), это лишь масштаб одного компонента).
		ApplyVolumeVisuals();
		break;
	}

	default:
		break;
	}
}

void ARenderCullVolume::EndGizmoDrag()
{
	if (ActiveGizmoHandle == EVolumeGizmoHandle::None)
	{
		return;
	}

	ActiveGizmoHandle = EVolumeGizmoHandle::None;
	ActiveGizmoAxis = FVector::ZeroVector;

	// Программный SetActorLocation()/SetBoxExtent() не триггерит PostEditMove()
	// (тот editor-only и реагирует только на ручной драг в редакторе), так что
	// без явного вызова клетки остались бы отсечёнными по старым границам до
	// следующего шага симуляции - ровно та же причина, по которой его зовёт
	// MoveCullVolumeToSelection().
	NotifyOrchestratorToRefresh();
}

FBox ARenderCullVolume::GetWorldBounds() const
{
	const FVector Origin = BoundsBox->GetComponentLocation();
	const FVector Extent = BoundsBox->GetScaledBoxExtent();
	return FBox(Origin - Extent, Origin + Extent);
}

void ARenderCullVolume::NotifyOrchestratorToRefresh() const
{
	if (AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass())))
	{
		Orchestrator->RefreshRenderCullVolume();
	}
}

#if WITH_EDITOR
void ARenderCullVolume::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	// bFinished==false - промежуточные тики драга, ещё не отпустили мышь;
	// перерисовывать на каждый из них незачем (та же дорогая AddInstances,
	// от которой мы и пытаемся уйти) - ждём, пока драг реально завершится.
	if (bFinished)
	{
		NotifyOrchestratorToRefresh();
	}
}

void ARenderCullVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Ловит и правку BoxExtent числами (масштаб залитого меша надо пересчитать),
	// и назначение самого VolumeMaterial - куб должен покраситься сразу в
	// редакторе, не только после запуска PIE.
	ApplyVolumeVisuals();

	NotifyOrchestratorToRefresh();
}
#endif
