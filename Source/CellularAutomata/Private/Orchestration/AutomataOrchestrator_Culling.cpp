// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Automata/Selection/CellSelection.h"
#include "Automata/Meshing/ChunkGridView.h"
#include "Kismet/GameplayStatics.h"


bool AAutomataOrchestrator::ShouldRefreshViewSlice() const
{
	if (!bEnableViewSlice)
	{
		return false;
	}

	FVector Location;
	FVector Forward;
	if (!GetCameraView(Location, Forward))
	{
		return false;
	}

	if (!bHasViewSliceCameraState)
	{
		return true;
	}

	if (FVector::Dist(Location, LastViewSliceCameraLocation) > ViewSliceCameraMoveThreshold)
	{
		return true;
	}

	// Поворот сравнивается через скалярное произведение направлений, а не
	// через разницу углов Эйлера: у последних есть разрывы (переход через
	// 360, gimbal-эффекты у pitch), из-за которых порог срабатывал бы то
	// впустую, то не срабатывал вовсе.
	const float CosThreshold = FMath::Cos(FMath::DegreesToRadians(ViewSliceRotationThreshold));
	return FVector::DotProduct(Forward, LastViewSliceCameraForward) < CosThreshold;
}

void AAutomataOrchestrator::SetViewSliceEnabled(bool bEnabled)
{
	bEnableViewSlice = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("SetViewSliceEnabled: срез вдоль взгляда %s"), bEnableViewSlice ? TEXT("включён") : TEXT("выключен"));
	ShowStatusMessage(StatusKey_ViewSlice, bEnableViewSlice
		? FString::Printf(TEXT("[J] Срез вдоль взгляда ВКЛ  -  середина %.0f, толщина %.0f  ([ ] двигать, Shift+[ ] толщина)"), ViewSliceDistance, ViewSliceThickness)
		: FString(TEXT("[J] Срез вдоль взгляда ВЫКЛ")));

	// Тик нужен самому срезу, а не только симуляции: он следит за камерой
	// (см. ShouldRefreshViewSlice() в Tick()), а разглядывают структуру как
	// раз на паузе, когда актор иначе не тикал бы вообще
	// (bStartWithTickEnabled = false, включает только Start()). Выключая срез,
	// возвращаем тик тому, кто в нём ещё нуждается.
	SetActorTickEnabled(bEnableViewSlice || bSimulationRunning || bFastStepActive);

	// При включении сбрасываем запомненное положение камеры - иначе первый
	// Tick() сравнил бы с состоянием от прошлого включения и мог решить, что
	// перестраивать не нужно.
	bHasViewSliceCameraState = false;
	// Немедленно, а не со следующим поколением - на паузе следующего может и
	// не быть (та же причина, что у SetRenderCullVolumeEnabled()).
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::SetAgeFilter(int32 NewAgeFilter, bool bIncludeOlder)
{
	AgeFilterValues.Reset();
	if (NewAgeFilter >= 0)
	{
		AgeFilterValues.Add(FMath::Clamp(NewAgeFilter, 0, 255));
	}

	// При снятом фильтре флаг бессмыслен, и оставленный включённым он путал бы
	// и Details-панель, и следующее нажатие цифры.
	bAgeFilterIncludesOlder = AgeFilterValues.Num() > 0 && bIncludeOlder;

	ApplyAgeFilterChange();
}

void AAutomataOrchestrator::ToggleAgeFilterValue(int32 Age, bool bIncludeOlder)
{
	if (Age < 0 || Age > 255)
	{
		UE_LOG(LogTemp, Warning, TEXT("ToggleAgeFilterValue: возраст %d вне диапазона 0..255"), Age);
		return;
	}

	if (AgeFilterValues.Remove(Age) > 0)
	{
		// Флаг "и всё, что старше" принадлежит той цифре, которая его подняла,
		// и уходит вместе с ней - иначе Shift+9 убрал бы девятку, но оставил
		// висеть весь хвост рампы, что выглядит как "не сработало".
		if (bIncludeOlder)
		{
			bAgeFilterIncludesOlder = false;
		}
	}
	else
	{
		AgeFilterValues.Add(Age);
		if (bIncludeOlder)
		{
			bAgeFilterIncludesOlder = true;
		}
	}

	// Убрали последний выбранный возраст - фильтра больше нет, а значит нет и
	// хвоста: пустой список и "показывать все" - одно состояние, а не два.
	if (AgeFilterValues.Num() == 0)
	{
		bAgeFilterIncludesOlder = false;
	}

	ApplyAgeFilterChange();
}

void AAutomataOrchestrator::ApplyAgeFilterChange()
{
	// Канонизация нужна не только после ручной правки списка в Details-панели:
	// от неё зависит и порядок в сообщении на экране, и то, что повторное
	// Shift+цифра действительно находит уже добавленный возраст.
	for (int32 Index = AgeFilterValues.Num() - 1; Index >= 0; --Index)
	{
		const int32 Value = AgeFilterValues[Index];
		if (Value < 0 || Value > 255)
		{
			AgeFilterValues.RemoveAt(Index);
		}
	}

	AgeFilterValues.Sort();

	for (int32 Index = AgeFilterValues.Num() - 1; Index > 0; --Index)
	{
		if (AgeFilterValues[Index] == AgeFilterValues[Index - 1])
		{
			AgeFilterValues.RemoveAt(Index);
		}
	}

	const FString Description = DescribeAgeFilter();

	// Строки собираются заранее, а не тернарником внутри Printf(): формат-строка
	// проверяется на этапе компиляции (consteval TCheckedFormatString) и обязана
	// быть литералом, а не выбранным во время исполнения указателем.
	FString LogText(TEXT("фильтр снят, показываются все клетки"));
	FString StatusText(TEXT("Фильтр по возрасту снят"));
	if (AgeFilterValues.Num() > 0)
	{
		LogText = FString::Printf(TEXT("показываются только клетки: %s"), *Description);
		StatusText = FString::Printf(TEXT("Возраст: %s  (Shift+цифра - добавить/убрать, та же цифра - показать все)"), *Description);
	}

	UE_LOG(LogTemp, Log, TEXT("SetAgeFilter: %s"), *LogText);
	ShowStatusMessage(StatusKey_AgeFilter, StatusText);
	RefreshRenderCullVolume();
}

FString AAutomataOrchestrator::DescribeAgeFilter() const
{
	if (AgeFilterValues.Num() == 0)
	{
		return TEXT("все возрасты");
	}

	FString Result;
	for (int32 Index = 0; Index < AgeFilterValues.Num(); ++Index)
	{
		if (Index > 0)
		{
			Result += TEXT(", ");
		}
		Result += FString::FromInt(AgeFilterValues[Index]);
	}

	// Хвост относится к самому старому из выбранных - только он и может быть
	// открытым сверху.
	if (bAgeFilterIncludesOlder)
	{
		Result += TEXT(" и старше");
	}

	return Result;
}

bool AAutomataOrchestrator::BuildAgeFilterMask(TArray<bool>& OutMask) const
{
	if (AgeFilterValues.Num() == 0)
	{
		return false;
	}

	OutMask.Init(false, 256);

	int32 MaxSelected = 0;
	for (const int32 Value : AgeFilterValues)
	{
		if (Value < 0 || Value > 255)
		{
			continue;
		}

		OutMask[Value] = true;
		MaxSelected = FMath::Max(MaxSelected, Value);
	}

	// "И всё, что старше" открывает диапазон вверх от самого старого из
	// выбранных: цифр десять, а возрастов 256, и без этого хвост рампы не
	// показывался бы ни под какой цифрой.
	if (bAgeFilterIncludesOlder)
	{
		for (int32 Age = MaxSelected; Age < 256; ++Age)
		{
			OutMask[Age] = true;
		}
	}

	return true;
}

void AAutomataOrchestrator::AdjustViewSliceDistance(float Delta)
{
	ViewSliceDistance = FMath::Max(ViewSliceDistance + Delta, 0.0f);
	UE_LOG(LogTemp, Log, TEXT("AdjustViewSliceDistance: середина среза на %.0f от камеры"), ViewSliceDistance);
	// Сообщение показывается и когда срез выключен: иначе нажатие [ / ] при
	// выключенном срезе выглядело бы как "клавиша не работает", хотя значение
	// исправно меняется и подействует при включении.
	ShowStatusMessage(StatusKey_ViewSlice, FString::Printf(TEXT("[%s] Срез: середина %.0f, толщина %.0f%s"),
		Delta < 0.0f ? TEXT("[") : TEXT("]"), ViewSliceDistance, ViewSliceThickness,
		bEnableViewSlice ? TEXT("") : TEXT("   (срез ВЫКЛЮЧЕН - включить J)")));
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::AdjustViewSliceThickness(float Delta)
{
	ViewSliceThickness = FMath::Max(ViewSliceThickness + Delta, 1.0f);
	UE_LOG(LogTemp, Log, TEXT("AdjustViewSliceThickness: толщина среза %.0f"), ViewSliceThickness);
	// См. одноимённый комментарий в AdjustViewSliceDistance().
	ShowStatusMessage(StatusKey_ViewSlice, FString::Printf(TEXT("[Shift+%s] Срез: середина %.0f, толщина %.0f%s"),
		Delta < 0.0f ? TEXT("[") : TEXT("]"), ViewSliceDistance, ViewSliceThickness,
		bEnableViewSlice ? TEXT("") : TEXT("   (срез ВЫКЛЮЧЕН - включить J)")));
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::ApplyCellCullDistances()
{
	// Отсечение по расстоянию (не HLOD - см. doc-comment CellCullEndDistance
	// в заголовке) применяем к ОБОИМ компонентам одинаково, а не только к
	// активному - если CellMeshComponentType переключат позже, второй
	// компонент не должен остаться со старыми (или дефолтными) значениями.
	// SetCullDistances() сама no-op, если значения не изменились, так что
	// звать её лишний раз дёшево. Пока bEnableCellCulling == false -
	// применяем (0, 0), не трогая сами CellCullStartDistance/CellCullEndDistance,
	// чтобы выключение хоткеем B не сбрасывало подобранные числа.
	//
	// Отдельная функция (не встроена в рендер): SetCullDistances() обновляет
	// уже существующий SceneProxy на Render Thread немедленно (см.
	// UInstancedStaticMeshComponent::SetCullDistances()) - ему не нужен новый
	// AddInstances()/рендер, чтобы подействовать. SetCellCullingEnabled()
	// зовёт эту функцию сама, сразу, без ожидания следующего рендера.
	const int32 CullStart = bEnableCellCulling ? FMath::Max(0, FMath::RoundToInt(CellCullStartDistance)) : 0;
	const int32 CullEnd = bEnableCellCulling ? FMath::Max(0, FMath::RoundToInt(CellCullEndDistance)) : 0;

	// Логируем только на фактическое изменение (не на каждый вызов) -
	// сверяемся с уже применённым значением на компоненте, а не храним
	// отдельное поле-кэш. Именно это "начало отсечения": сам движок решает,
	// какие конкретно инстансы не рисовать, каждый кадр и без обратной связи
	// в C++ - здесь мы можем зафиксировать только момент, когда порог
	// (Start/End) поменялся, т.е. отсечение включилось/выключилось/сдвинулось.
	int32 PrevCullStart = 0;
	int32 PrevCullEnd = 0;
	CellsMeshHierarchical->GetCullDistances(PrevCullStart, PrevCullEnd);
	if (PrevCullStart != CullStart || PrevCullEnd != CullEnd)
	{
		if (CullEnd > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("ApplyCellCullDistances: отсечение клеток по расстоянию включено (Start=%d, End=%d)"), CullStart, CullEnd);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("ApplyCellCullDistances: отсечение клеток по расстоянию выключено"));
		}
	}

	CellsMeshHierarchical->SetCullDistances(CullStart, CullEnd);
	CellsMeshFlat->SetCullDistances(CullStart, CullEnd);

	if (SelectionMeshComponent)
	{
		SelectionMeshComponent->SetCullDistances(CullStart, CullEnd);
	}

	// Диагностика для отладки отсечения - пока порог включён (CullEnd > 0),
	// раз в секунду (не на каждый вызов - при высоком Speed это был бы спам)
	// печатаем всё, что нужно, чтобы понять, ПОЧЕМУ клетки не отсекаются:
	// позицию камеры, центр/радиус текущей сетки, реальное расстояние между
	// ними и то, что фактически осело на компоненте после SetCullDistances()
	// (не просто то, что мы передали - вдруг движок не принял значение).
	if (CullEnd > 0)
	{
		static double LastCullDebugLogSeconds = 0.0;
		const double NowSeconds = FPlatformTime::Seconds();
		if (NowSeconds - LastCullDebugLogSeconds >= 1.0)
		{
			LastCullDebugLogSeconds = NowSeconds;

			FVector CameraLocation = FVector::ZeroVector;
			const bool bHaveCamera = (GamePC != nullptr && GamePC->PlayerCameraManager != nullptr);
			if (bHaveCamera)
			{
				CameraLocation = GamePC->PlayerCameraManager->GetCameraLocation();
			}

			FVector GridCenter = FVector::ZeroVector;
			float GridRadius = 0.0f;
			const bool bHaveBounds = ComputeAliveCellsBounds(GridCenter, GridRadius);

			const float DistanceToCenter = (bHaveCamera && bHaveBounds)
				? FVector::Dist(CameraLocation, GridCenter)
				: -1.0f;

			int32 ActualStart = 0;
			int32 ActualEnd = 0;
			CellsMeshHierarchical->GetCullDistances(ActualStart, ActualEnd);

			UE_LOG(LogTemp, Log, TEXT("ApplyCellCullDistances: [cull debug] камера=%s (есть=%d), центр сетки=%s радиус=%.1f (есть=%d), расстояние камера-центр=%.1f, задано Start/End=%d/%d, реально на CellsMeshHierarchical Start/End=%d/%d"),
				*CameraLocation.ToString(), bHaveCamera ? 1 : 0,
				*GridCenter.ToString(), GridRadius, bHaveBounds ? 1 : 0,
				DistanceToCenter,
				CullStart, CullEnd,
				ActualStart, ActualEnd);
		}
	}
}

bool AAutomataOrchestrator::MoveCullVolumeToChunkUnderCursor(const FVector& RayOrigin, const FVector& RayDirection)
{
	if (!Grid)
	{
		return false;
	}

	const FVector ChunkWorldExtent = Grid->GetChunkWorldExtent();
	if (ChunkWorldExtent.GetMin() <= 0.0)
	{
		// Сетка без чанков - выбирать нечего (см. FCellGrid::GetChunkWorldExtent()).
		return false;
	}

	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	if (!CullVolume)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeToChunkUnderCursor: на уровне нет ARenderCullVolume - разместите его сначала"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("На уровне нет ARenderCullVolume - разместите его"));
		return false;
	}

	FVector BoundsCenter = FVector::ZeroVector;
	float BoundsRadius = 0.0f;
	if (!ComputeAliveCellsBounds(BoundsCenter, BoundsRadius))
	{
		return false;
	}
	const double MaxDistance = FVector::Distance(RayOrigin, BoundsCenter) + BoundsRadius + ChunkWorldExtent.GetMax();

	// Тот же DDA, что ищет клетку под курсором - он принимает абстрактный
	// FCellGrid и не знает, клетки в нём или чанки. FChunkGridView - это и
	// есть сетка из чанков (её же строит гост-силуэт), только здесь она
	// нужна с НАСТОЯЩИМ IsAlive(): иначе луч вернул бы первый задетый чанк,
	// включая пустые (см. bBuildOccupancySet в её конструкторе).
	TArray<FIntVector> OccupiedChunks;
	Grid->GetOccupiedChunkCoords(OccupiedChunks);
	if (OccupiedChunks.Num() == 0)
	{
		return false;
	}

	const FChunkGridView ChunkView(ChunkWorldExtent, Grid->GetLattice().GetCellWorldExtent(), MoveTemp(OccupiedChunks), /*bBuildOccupancySet=*/true);

	FIntVector PickedChunk;
	if (!CellSelection::PickCellAlongRay(ChunkView, RayOrigin, RayDirection, MaxDistance, PickedChunk))
	{
		ShowStatusMessage(StatusKey_CullVolume, TEXT("Клик мимо - под курсором нет занятых чанков"));
		return true;
	}

	// GridToWorld() у этой вьюхи специально возвращает ЦЕНТР чанка, а не его
	// угол (см. её doc-comment) - то есть ровно то, во что надо поставить куб.
	const FVector ChunkCenter = ChunkView.GridToWorld(PickedChunk);
	CullVolume->SetActorLocation(ChunkCenter);

	UE_LOG(LogTemp, Log, TEXT("MoveCullVolumeToChunkUnderCursor: куб отсечения переставлен на чанк %s (мир: %s)"),
		*PickedChunk.ToString(), *ChunkCenter.ToString());
	ShowStatusMessage(StatusKey_CullVolume, FString::Printf(
		TEXT("Куб отсечения на чанк %s.  H - убрать силуэт, C - включить отсечение"), *PickedChunk.ToString()));

	// SetActorLocation() программно не поднимает PostEditMove() - перерисовываем
	// сами, как в MoveCullVolumeToSelection().
	RefreshRenderCullVolume();
	return true;
}

ARenderCullVolume* AAutomataOrchestrator::EnsureRenderCullVolume()
{
	if (!IsValid(CachedRenderCullVolume))
	{
		CachedRenderCullVolume = Cast<ARenderCullVolume>(UGameplayStatics::GetActorOfClass(GetWorld(), ARenderCullVolume::StaticClass()));
	}
	return CachedRenderCullVolume;
}

ARenderCullVolume* AAutomataOrchestrator::GetActiveCullVolume()
{
	if (!bEnableRenderCullVolume)
	{
		return nullptr;
	}

	// Видимость куба на отсечение НЕ влияет - см. doc-comment в заголовке.
	return EnsureRenderCullVolume();
}

void AAutomataOrchestrator::SetCellCullingEnabled(bool bEnabled)
{
	bEnableCellCulling = bEnabled;
	// Настройка принадлежит профилю рендера - раз её тронули руками, профиль в
	// HUD больше не описывает то, что на экране (см. FHudStats::bRenderPresetModified).
	bRenderPresetModified = true;
	UE_LOG(LogTemp, Log, TEXT("SetCellCullingEnabled: отсечение клеток по расстоянию %s"), bEnabled ? TEXT("включено") : TEXT("выключено"));

	// Применяем немедленно, не дожидаясь следующего рендера (см. doc-comment
	// ApplyCellCullDistances()) - иначе переключение хоткеем B, пока новое
	// поколение не рендерится, визуально ничего не меняло до следующего шага.
	ApplyCellCullDistances();
}

void AAutomataOrchestrator::SetRenderCullVolumeEnabled(bool bEnabled)
{
	bEnableRenderCullVolume = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("SetRenderCullVolumeEnabled: отсечение по ARenderCullVolume %s"), bEnabled ? TEXT("включено") : TEXT("выключено"));
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::RefreshRenderCullVolume()
{
	if (!Grid)
	{
		// Ещё нет сетки (до первого GenerateRandom()) - перерисовывать
		// нечего, следующий GenerateRandom()/Next() и так учтёт актуальные
		// границы куба сам.
		return;
	}

	// В отличие от ApplyCellCullDistances() (который просто перевызывает
	// SetCullDistances() на уже построенных инстансах), изменение куба
	// меняет САМ набор клеток, попадающих в AddInstances - недостаточно
	// применить настройку "на лету" без полного набора инстансов, нужно
	// заново пройти BuildCellRenderData()/AddInstances() для текущего состояния
	// (не считая новое поколение - RenderGridImmediate() рендерит уже
	// посчитанный Grid как есть, тот же путь, что Next()/GenerateRandom()).
	// Иначе переключение хоткеем C или перетаскивание ARenderCullVolume
	// визуально ничего не меняло бы до следующего шага симуляции - ровно
	// то же соображение, что и у SetCellCullingEnabled() выше.
	RenderGridImmediate();

	// Ghost Shape отсекает по тем же границам куба (см. RefreshGhostShape()) -
	// без этого вызова передвинутый/ресайзнутый куб оставлял бы старый
	// ghost-силуэт висеть до истечения GhostShapeRefreshInterval поколений
	// (могло потребовать "прокрутить несколько эпох", прежде чем форма
	// подстроится). PostEditMove(bFinished=true)/PostEditChangeProperty на
	// ARenderCullVolume уже сами по себе - естественный дебаунс: событие
	// приходит один раз по завершении перетаскивания/правки, а не на каждый
	// промежуточный тик драга.
	if (bEnableGhostShape)
	{
		GhostShapeGenerationsSinceRefresh = 0;
		RefreshGhostShape();
	}
}

void AAutomataOrchestrator::MoveCullVolumeToSelection()
{
	// Все отказы ниже сообщаются и на экран, а не только в лог: без этого
	// нажатие K с пустым выделением выглядит как сломанная клавиша - ровно
	// та же жалоба, что была про срез вдоль взгляда.
	if (SelectedCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeToSelection: выделение пусто - сначала выделите клетку (Tab, затем ЛКМ)"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("[K] Выделение пусто - сначала Tab, затем ЛКМ по клетке"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeToSelection: сетка не инициализирована"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("[K] Сетка не инициализирована"));
		return;
	}

	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	if (!CullVolume)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeToSelection: на уровне нет ARenderCullVolume - разместите его сначала"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("[K] На уровне нет ARenderCullVolume - разместите его"));
		return;
	}

	// Только первая выделенная клетка - куб один, центрировать его
	// одновременно на нескольких точках невозможно, а первая обычно и есть
	// та, с которой начали выделение/клик.
	const FIntVector& TargetCell = SelectedCells[0];
	const FVector TargetLocation = Grid->GridToWorld(TargetCell);
	CullVolume->SetActorLocation(TargetLocation);

	UE_LOG(LogTemp, Log, TEXT("MoveCullVolumeToSelection: куб отсечения перемещён к клетке %s (мир: %s)"),
		*TargetCell.ToString(), *TargetLocation.ToString());
	// Отдельно сообщаем, если куб сейчас не режет: он честно переехал, но на
	// экране ничего не изменится, и это выглядело бы как несработавшая
	// клавиша. Условие берём из GetActiveCullVolume() - там же, где его
	// проверяет рендер, чтобы сообщение не разошлось с поведением.
	const bool bCullingActive = GetActiveCullVolume() != nullptr;
	ShowStatusMessage(StatusKey_CullVolume, FString::Printf(TEXT("[K] Куб отсечения по центру клетки %s%s"),
		*TargetCell.ToString(),
		bCullingActive ? TEXT("") : TEXT("   (отсечение НЕ активно - включить на C)")));

	// SetActorLocation() программно не триггерит ARenderCullVolume::
	// PostEditMove() (WITH_EDITOR-only, реагирует только на ручное
	// перетаскивание/правку в Details panel) - перерисовываем сами, тем же
	// путём, что и хоткей C/сам PostEditMove().
	RefreshRenderCullVolume();
}

void AAutomataOrchestrator::MoveCullVolumeByCells(const FIntVector& CellDelta)
{
	if (CellDelta.IsZero())
	{
		return;
	}

	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	if (!CullVolume)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveCullVolumeByCells: на уровне нет ARenderCullVolume - разместите его сначала"));
		ShowStatusMessage(StatusKey_CullVolume, TEXT("Стрелки: на уровне нет ARenderCullVolume - разместите его"));
		return;
	}

	// Через решётку, а не умножением на CellSize вручную: сдвиг на клетку
	// вдоль растянутой оси длиннее, чем в плоскости, иначе куб уезжал бы на
	// полклетки и вставал между слоями. GridDeltaToWorld(), а не
	// GridToWorld(), потому что это РАЗНОСТЬ - начало координат решётки в ней
	// обязано сократиться.
	const FVector WorldDelta = Grid ? Grid->GetLattice().GridDeltaToWorld(CellDelta) : FVector(CellDelta) * CellSize;
	const FVector NewLocation = CullVolume->GetActorLocation() + WorldDelta;
	CullVolume->SetActorLocation(NewLocation);

	// Как и в MoveCullVolumeToSelection(): программный SetActorLocation() не
	// поднимает PostEditMove(), так что перерисовываем сами. Сообщение тоже
	// оттуда - куб мог переехать, но если отсечение не активно, на экране
	// ничего не изменится, и это неотличимо от несработавшей клавиши.
	const bool bCullingActive = GetActiveCullVolume() != nullptr;
	ShowStatusMessage(StatusKey_CullVolume, FString::Printf(TEXT("Куб отсечения: %s%s"),
		*NewLocation.ToCompactString(),
		bCullingActive ? TEXT("") : TEXT("   (отсечение НЕ активно - включить на C)")));

	RefreshRenderCullVolume();
}

const FBox* AAutomataOrchestrator::GetActiveCullBounds(FBox& OutBounds)
{
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	if (!CullVolume)
	{
		return nullptr;
	}

	OutBounds = CullVolume->GetWorldBounds();
	return &OutBounds;
}

CellVisibility::FFilter AAutomataOrchestrator::BuildVisibilityFilter(bool bUpdateSliceCameraState)
{
	// Инициализированы явно: GetCameraView() пишет их только при успехе, и
	// хотя читаются они строго под bSliceActive, компилятор этого не выводит.
	FVector SliceOrigin = FVector::ZeroVector;
	FVector SliceForward = FVector::ForwardVector;
	const bool bSliceActive = bEnableViewSlice && GetCameraView(SliceOrigin, SliceForward);

	if (bSliceActive && bUpdateSliceCameraState)
	{
		// Запоминаем, для какой камеры срез построен - по этому состоянию
		// Tick() решает, пора ли перестраивать (см. ShouldRefreshViewSlice()).
		LastViewSliceCameraLocation = SliceOrigin;
		LastViewSliceCameraForward = SliceForward;
		bHasViewSliceCameraState = true;
	}

	CellVisibility::FFilter Filter = CellVisibility::MakeSliceFilter(
		bSliceActive, SliceOrigin, SliceForward, ViewSliceDistance, ViewSliceThickness);

	Filter.bAgeFilterActive = BuildAgeFilterMask(Filter.AgeMask);
	return Filter;
}
