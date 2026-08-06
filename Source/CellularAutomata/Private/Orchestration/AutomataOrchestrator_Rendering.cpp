// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Automata/Rendering/RenderPresets.h"
#include "Automata/Simulation/CellDecay.h"
#include "Engine/Engine.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "EngineUtils.h"

// Сглаженный FPS движка - определён в UnrealEngine.cpp, без публичного
// заголовка, объявляется локально там, где используется (тот же паттерн,
// что и в самом движке, см. напр. EngineAnalyticsSessionSummary.cpp) - см.
// FHudStats::CurrentFPS в Tick().
extern ENGINE_API float GAverageFPS;

namespace
{
	/** Печатает разбивку FRenderTimings по фазам. До этого все шесть таймеров
	 *  исправно набирались и молча выбрасывались - GetLastRenderTimings() не
	 *  звал никто, и ChunkedRenderCellsPerFrame подбирался на глаз по
	 *  заиканиям.
	 *
	 *  Фазы намеренно разделены на две группы, а не сложены в одну сумму:
	 *  Transforms/AddInstances/CustomData набираются ПОЧАНКОВО и потому
	 *  размазаны по кадрам разлива - только из них имеет смысл считать
	 *  цену клетки и цену кадра, т.е. ровно те две величины, по которым
	 *  подбирается ChunkedRenderCellsPerFrame. SetMesh/Clear/Scale/Reorder
	 *  случаются РАЗОМ внутри BeginRender() на первом кадре и по кадрам не
	 *  делятся вовсе, так что усреднять их вместе с остальными - значит
	 *  занижать первый кадр и завышать все следующие. Reorder тут особенно
	 *  интересен: Algo::Sort по миллионам инстансов в один поток может
	 *  оказаться дороже всего разлива, и никакой бюджет на кадр его не
	 *  размажет.
	 *
	 *  RenderedCells - это LastRenderStats.RenderedCellCount, т.е. число
	 *  клеток ПОСЛЕ отсечения кубом (ARenderCullVolume): именно оно уходит в
	 *  AddInstances и именно им управляет ChunkedRenderCellsPerFrame, а не
	 *  Grid->Num(). */
	void LogRenderTimings(const TCHAR* Context, const FRenderTimings& Timings, int32 RenderedCells, int32 FrameCount)
	{
		const double ChunkedSeconds = Timings.BuildTransformsSeconds + Timings.AddInstanceSeconds + Timings.CustomDataSeconds;
		const double SetupSeconds = Timings.SetMeshSeconds + Timings.ClearSeconds + Timings.ScaleSeconds + Timings.ReorderSeconds;

		const int32 SafeFrameCount = FMath::Max(FrameCount, 1);
		const double PerCellMicroseconds = (RenderedCells > 0) ? ChunkedSeconds * 1000000.0 / RenderedCells : 0.0;
		const double PerFrameMs = ChunkedSeconds * 1000.0 / SafeFrameCount;

		// FPS в той же строке, потому что вставка инстансов и их отрисовка -
		// разные стороны одного выбора: HISM строит дерево кластеров (дороже
		// вставка), но получает окклюзию и отсечение по кластерам (дешевле
		// отрисовка). Сравнивать CellMeshComponentType по одному лишь
		// AddInstances бессмысленно, а на глаз - тем более.
		UE_LOG(LogTemp, Log, TEXT("RenderTimings[%s]: клеток %d | FPS %.1f | разлив %.2f мс = %.3f мкс/клетка, %.2f мс/кадр за %d кадр(ов) [Transforms %.2f / AddInstances %.2f / CustomData %.2f] | разово в BeginRender %.2f мс [SetMesh %.2f / Clear %.2f / Scale %.2f / Reorder %.2f]"),
			Context, RenderedCells, GAverageFPS,
			ChunkedSeconds * 1000.0, PerCellMicroseconds, PerFrameMs, SafeFrameCount,
			Timings.BuildTransformsSeconds * 1000.0, Timings.AddInstanceSeconds * 1000.0, Timings.CustomDataSeconds * 1000.0,
			SetupSeconds * 1000.0,
			Timings.SetMeshSeconds * 1000.0, Timings.ClearSeconds * 1000.0,
			Timings.ScaleSeconds * 1000.0, Timings.ReorderSeconds * 1000.0);
	}
}

UInstancedStaticMeshComponent* AAutomataOrchestrator::GetActiveCellsMeshComponent() const
{
	return (CellMeshComponentType == ECellMeshComponentType::HierarchicalInstanced)
		? static_cast<UInstancedStaticMeshComponent*>(CellsMeshHierarchical)
		: static_cast<UInstancedStaticMeshComponent*>(CellsMeshFlat);
}

void AAutomataOrchestrator::ClearInactiveCellsMeshComponent()
{
	UInstancedStaticMeshComponent* InactiveComponent =
		(CellMeshComponentType == ECellMeshComponentType::HierarchicalInstanced)
			? static_cast<UInstancedStaticMeshComponent*>(CellsMeshFlat)
			: static_cast<UInstancedStaticMeshComponent*>(CellsMeshHierarchical);

	if (InactiveComponent && InactiveComponent->GetInstanceCount() > 0)
	{
		InactiveComponent->ClearInstances();
	}
}

void AAutomataOrchestrator::EnsureCellsRenderer()
{
	UInstancedStaticMeshComponent* Target = GetActiveCellsMeshComponent();
	if (!Target)
	{
		return;
	}

	// Одно условие на три случая: рендерера ещё нет (первый вызов либо
	// обнуление после реинстансинга Live Coding - сами компоненты default
	// subobject'ы и переживают его, а TUniquePtr нет), либо он обёрнут вокруг
	// другого компонента (поменяли CellMeshComponentType).
	if (CellsRenderer && CellsRenderer->GetComponent() == Target)
	{
		return;
	}

	// Перепривязка: прежний компонент обязан остаться пустым, иначе его
	// инстансы висят внахлёст с новыми.
	if (CellsRenderer)
	{
		if (UInstancedStaticMeshComponent* PreviousComponent = CellsRenderer->GetComponent())
		{
			PreviousComponent->ClearInstances();
		}
	}

	CellsRenderer = MakeUnique<FInstancedMeshCellGridRenderer>(Target);
}

FLinearColor AAutomataOrchestrator::SampleColorRamp(const TArray<FLinearColor>& Keys, float T) const
{
	return ColorRamp::Sample(Keys, T, ColorRampSpace, ColorRampCurve);
}

void AAutomataOrchestrator::BuildAgeColorLut(TArray<FColor>& OutLut, bool bSRGB) const
{
	OutLut.SetNumUninitialized(256);
	const float MaxAge = float(FMath::Max(1, AgeColorMaxAge));
	for (int32 Age = 0; Age < 256; ++Age)
	{
		// bSRGB=false обязательно: PerInstanceCustomData это сырой float,
		// материал никакого sRGB-декода не делает - гамма-кодирование здесь
		// тихо испортило бы всю рампу (см. FCellRenderInstance).
		OutLut[Age] = SampleColorRamp(AgeColors, float(Age) / MaxAge).ToFColor(bSRGB);
	}
}

void AAutomataOrchestrator::BuildDecayColorLut(TArray<FColor>& OutLut, bool bSRGB) const
{
	OutLut.SetNumUninitialized(256);

	// Пустой DecayColors - берём возрастную рампу, т.е. "как было до появления
	// отдельной шкалы угасания" (см. doc-comment DecayColors).
	const TArray<FLinearColor>& Ramp = (DecayColors.Num() > 0) ? DecayColors : AgeColors;

	// Стадии угасания - это [2 .. States-1]: 2 "только начала гаснуть",
	// States-1 "последняя стадия перед смертью". Итого States-2 стадий, а
	// значит States-3 интервалов между ними. При States == 3 стадия ровно
	// одна - знаменатель зажимаем в 1, T выходит 0, берётся первый ключ.
	const int32 Denominator = FMath::Max(1, States - 3);
	for (int32 State = 0; State < 256; ++State)
	{
		const float T = float(FMath::Clamp(State - 2, 0, Denominator)) / float(Denominator);
		OutLut[State] = SampleColorRamp(Ramp, T).ToFColor(bSRGB);
	}
}

void AAutomataOrchestrator::ClearAllCellInstances()
{
	// Полный обход ВСЕХ реально прикреплённых к актору
	// UInstancedStaticMeshComponent, а не только тех, что перечислены в
	// CellsMeshFlat/CellsMeshHierarchical/SelectionMeshComponent - защита от
	// осиротевших компонентов: лишние компоненты остаются
	// physически прикреплены и видимы, продолжая рисовать свои старые
	// инстансы поверх честно посчитанных - visуально выглядит как
	// наложение/мерцание двух состояний, хотя логическое состояние
	// симуляции (Grid) при этом только одно. Обнаруженный на практике
	// случай (ещё во времена пула возрастных компонентов): материалов было 3,
	// а на акторе висело 8 InstancedStaticMeshComponent - 5 лишних,
	// ClearInstances() по одному только легитимному набору их не касался.
	//
	// После перехода на per-instance цвет этот же механизм заодно подчищает
	// сам бывший пул: легитимный набор сократился до трёх компонентов, и все
	// рантайм-созданные возрастные компоненты стали здесь сиротами.
	TArray<UInstancedStaticMeshComponent*> AllInstancedComponents;
	GetComponents<UInstancedStaticMeshComponent>(AllInstancedComponents);

	TSet<UInstancedStaticMeshComponent*> KeepSet;
	if (CellsMeshFlat)
	{
		KeepSet.Add(CellsMeshFlat);
	}
	if (CellsMeshHierarchical)
	{
		KeepSet.Add(CellsMeshHierarchical);
	}
	if (SelectionMeshComponent)
	{
		KeepSet.Add(SelectionMeshComponent);
	}

	for (UInstancedStaticMeshComponent* Comp : AllInstancedComponents)
	{
		if (!Comp)
		{
			continue;
		}

		if (KeepSet.Contains(Comp))
		{
			Comp->ClearInstances();
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ClearAllCellInstances: обнаружен и уничтожен осиротевший компонент %s (не входит в текущий легитимный набор)"), *Comp->GetName());
			Comp->DestroyComponent();
		}
	}
}

void AAutomataOrchestrator::BuildCellRenderData(TArray<FCellRenderInstance>& OutInstances)
{
	OutInstances.Reset();

	// Таблица цвета считается один раз на весь рендер, а не на клетку: при
	// миллионах клеток интерполяция в цикле - это миллионы лишних лерпов,
	// тогда как таблица занимает 1 КБ и даёт одно чтение по индексу.
	TArray<FColor> AgeLut;
	BuildAgeColorLut(AgeLut, bBuildingSliceCapture);

	TArray<FIntVector> AliveCells;

	// Если отсечение активно (см. GetActiveCullVolume()) - отсекаем клетки вне
	// границ куба ДО построения инстансов/трансформов, иначе рендерим всё
	// как раньше.
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	if (CullVolume)
	{
		Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), AliveCells);
	}
	else
	{
		Grid->GetAliveCells(AliveCells);
	}

	// Срез вдоль взгляда - см. bEnableViewSlice. Плоскость среза
	// перпендикулярна направлению камеры, поэтому проверка на клетку это одно
	// скалярное произведение: глубина вдоль взгляда против диапазона.
	// Считается ЗДЕСЬ, а не в рендерере, по той же причине, что и куб: клетки
	// вне среза не должны стоить построения трансформа.
	// Инициализированы явно: GetCameraView() пишет их только при успехе, и
	// хотя читаются они строго под bSliceActive, компилятор этого не выводит.
	FVector SliceOrigin = FVector::ZeroVector;
	FVector SliceForward = FVector::ForwardVector;
	const bool bSliceActive = bEnableViewSlice && GetCameraView(SliceOrigin, SliceForward);
	const float SliceMinDepth = ViewSliceDistance - ViewSliceThickness * 0.5f;
	const float SliceMaxDepth = ViewSliceDistance + ViewSliceThickness * 0.5f;

	if (bSliceActive)
	{
		// Запоминаем, для какой камеры срез построен - по этому состоянию
		// Tick() решает, пора ли перестраивать (см. ShouldRefreshViewSlice()).
		LastViewSliceCameraLocation = SliceOrigin;
		LastViewSliceCameraForward = SliceForward;
		bHasViewSliceCameraState = true;
	}

	// Фильтр по возрасту (см. AgeFilterValues) разворачивается в маску ДО
	// цикла: внутри тогда остаётся одно чтение из таблицы, без перебора
	// выбранных возрастов на каждой из миллионов клеток.
	TArray<bool> AgeFilterMask;
	const bool bAgeFilterActive = BuildAgeFilterMask(AgeFilterMask);

	OutInstances.Reserve(AliveCells.Num());
	for (const FIntVector& Cell : AliveCells)
	{
		const uint8 Age = Grid->GetAge(Cell);
		// Раньше остальных проверок: отсекает больше всего и обходится одним
		// чтением. Возраст 0 - законный слой, выключенному фильтру
		// соответствует пустой список, а не нулевой возраст.
		if (bAgeFilterActive && !AgeFilterMask[Age])
		{
			continue;
		}

		const FVector World = Grid->GridToWorld(Cell);
		if (bSliceActive)
		{
			const double Depth = FVector::DotProduct(World - SliceOrigin, SliceForward);
			if (Depth < SliceMinDepth || Depth > SliceMaxDepth)
			{
				continue;
			}
		}

		OutInstances.Add({ FVector3f(World), AgeLut[Age] });
	}

	// Generations (States > 2) - угасающие клетки (не живые, но ещё не
	// полностью мёртвые, см. FCellGrid::IsDecaying()) тоже нужно рисовать
	// (иначе они просто невидимы, хотя реально "занимают" клетку и угасают
	// на глазах у CellDecay::AdvanceDecayStates()). Цвет берётся из СВОЕЙ
	// таблицы (см. DecayColors) - раньше угасающие шли в те же возрастные
	// бакеты, что и живые, и были от них визуально неотличимы. При States == 2
	// этот блок вообще не выполняется - ни GetDecayingCells()/
	// GetDecayingCellsInBounds(), ни лишний проход, ни построение таблицы.
	// Фильтр по возрасту прячет угасающие клетки целиком: возраст у них не
	// определён - это отдельный канал состояния, а не возраст, и приписать им
	// какой-то возраст значило бы соврать (см. AgeFilterValues).
	if (States > 2 && !IsAgeFilterActive())
	{
		TArray<FColor> DecayLut;
		BuildDecayColorLut(DecayLut, bBuildingSliceCapture);

		TArray<FIntVector> DecayingCells;
		TArray<uint8> DecayingStates;
		if (CullVolume)
		{
			Grid->GetDecayingCellsInBounds(CullVolume->GetWorldBounds(), DecayingCells, DecayingStates);
		}
		else
		{
			Grid->GetDecayingCells(DecayingCells, DecayingStates);
		}

		OutInstances.Reserve(OutInstances.Num() + DecayingCells.Num());
		for (int32 Index = 0; Index < DecayingCells.Num(); ++Index)
		{
			const FVector World = Grid->GridToWorld(DecayingCells[Index]);
			// Тот же срез, что и для живых клеток выше - иначе угасающие
			// торчали бы сквозь него.
			if (bSliceActive)
			{
				const double Depth = FVector::DotProduct(World - SliceOrigin, SliceForward);
				if (Depth < SliceMinDepth || Depth > SliceMaxDepth)
				{
					continue;
				}
			}

			OutInstances.Add({ FVector3f(World), DecayLut[DecayingStates[Index]] });
		}
	}

	// Два разных вида числа тут (см. doc-comment FCellRenderStats):
	// RenderedCellCount/TotalCellCount - ПАРА, показывает масштаб расчётов
	// (сколько живых клеток реально отрисовано после отсечения
	// ARenderCullVolume против того, сколько их всего в сетке); а
	// EstimatedUploadMB - ОДНО общее число, оценка размера данных, которые
	// реально уходят в AddInstances() (не настоящий занятый VRAM - не
	// учитывает оверхед LOD-дерева HISM, ресурсы меша/материала, накладные
	// расходы драйвера, только сам TArray<FTransform>). Результат кладём в
	// LastRenderStats - UE_LOG ниже читает уже посчитанное оттуда, а не из
	// локальных переменных, чтобы будущий HUD (GetLastRenderStats()) видел
	// те же самые цифры, что и лог.
	//
	// RenderedCellCount берётся из ИТОГОВОГО массива, а не из AliveCells:
	// раньше сюда шло AliveCells.Num() уже ПОСЛЕ того, как угасающие клетки
	// были добавлены в бакеты и уходили в AddInstances - при States > 2
	// "отрисовано" систематически занижалось ровно на их число, а вместе с
	// ним и EstimatedUploadMB. Обратная сторона: теперь RenderedCellCount
	// может законно превышать TotalCellCount (Grid->Num() считает только
	// живых) - см. doc-comment FCellRenderStats.
	LastRenderStats.RenderedCellCount = OutInstances.Num();
	LastRenderStats.TotalCellCount = Grid->Num();
	LastRenderStats.BytesPerInstance = (int32)(sizeof(FTransform) + CellCustomDataFloats * sizeof(float));
	LastRenderStats.EstimatedUploadMB = (double(LastRenderStats.RenderedCellCount) * LastRenderStats.BytesPerInstance) / (1024.0 * 1024.0);

	UE_LOG(LogTemp, Log, TEXT("BuildCellRenderData: %d/%d клеток (отрисовано/живых в сетке) - выгрузка в AddInstances ~%.2f МБ (%d байт/инстанс: FTransform + %d float per-instance цвета, без учёта оверхеда HISM/драйвера)"),
		LastRenderStats.RenderedCellCount, LastRenderStats.TotalCellCount,
		LastRenderStats.EstimatedUploadMB, LastRenderStats.BytesPerInstance, CellCustomDataFloats);
}

void AAutomataOrchestrator::RenderGridImmediate()
{
	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("RenderGridImmediate: CellMaterial не назначен - рендер пропущен"));
		return;
	}

	EnsureCellsRenderer();
	ApplyCellCullDistances();
	ApplyCellShadowSettings();
	ClearInactiveCellsMeshComponent();

	if (!CellsRenderer)
	{
		return;
	}

	CellsRenderer->SetMesh(CellMesh);
	CellsRenderer->SetMaterial(EnsureCellMaterialInstance());
	// Задаётся явно на каждый рендер: тот же класс рендерера используется и для
	// подсветки выделения, где множитель свой (см. SelectionScaleMultiplier).
	// Инвариант "обычные клетки берут CellMeshScaleMultiplier" лучше держать
	// локально и видимо, чем полагаться на то, что этих двух рендереров никто
	// никогда не смешает.
	CellsRenderer->SetScaleMultiplier(CellMeshScaleMultiplier);

	if (ShouldGhostShapeReplaceDetailedRender())
	{
		// Ghost Shape уже покрывает всю сетку целиком (см. doc-comment
		// ShouldGhostShapeReplaceDetailedRender()) - пропускаем именно ту
		// дорогую работу (BuildCellRenderData()+AddInstances по каждой живой
		// клетке), ради которой эта фича существует. Через Render() с
		// пустым списком инстансов, а не сырой ClearInstances() на компоненте -
		// так внутренняя бухгалтерия рендерера (PendingInstances/PendingCursor)
		// остаётся согласованной с самим компонентом, вместо того чтобы её
		// обходить.
		CellsRenderer->Render(*Grid, TArray<FCellRenderInstance>());
		// Ноль - правда, а не отсутствие данных: детальных инстансов в
		// AddInstances() ушло ровно столько. Провал линии "видимо" в ноль при
		// включении Ghost Shape и есть та диагностика, ради которой график
		// делается.
		NoteRenderedCells(0);
		RenderSelectionOverlay();
		UE_LOG(LogTemp, Log, TEXT("RenderGridImmediate: детальный рендер пропущен - Ghost Shape покрывает всю сетку целиком (%d живых клеток)"),
			Grid->Num());
		return;
	}

	TArray<FCellRenderInstance> Instances;
	BuildCellRenderData(Instances);
	// До Render() ниже: там Instances уже перемещён.
	NoteRenderedCells(LastRenderStats.RenderedCellCount);

	// Всегда одним снимком (не BeginRender()/чанкинг) - Next()/GenerateRandom()
	// рендерят немедленно и целиком, независимо от bEnableChunkedRender
	// (см. doc-comment RenderGridImmediate() в заголовке).
	CellsRenderer->Render(*Grid, MoveTemp(Instances));

	// Не-op, если SelectedCells пуст (свежая сетка/шаг уже его сбросили) -
	// сам чистит SelectionMeshComponent в этом случае.
	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("RenderGridImmediate: живых клеток %d отрисовано одним снимком"),
		Grid->Num());
	// Один кадр по построению - "мс/кадр" здесь совпадает с полной ценой
	// разлива и показывает, во что обошёлся бы отказ от чанкинга.
	LogRenderTimings(TEXT("immediate"), CellsRenderer->GetLastRenderTimings(),
		LastRenderStats.RenderedCellCount, 1);
}

void AAutomataOrchestrator::RenderCurrentGrid()
{
	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("RenderCurrentGrid: CellMaterial не назначен - рендер пропущен"));
		return;
	}

	EnsureCellsRenderer();
	ApplyCellCullDistances();
	ApplyCellShadowSettings();
	ClearInactiveCellsMeshComponent();

	if (!CellsRenderer)
	{
		return;
	}

	CellsRenderer->SetMesh(CellMesh);
	CellsRenderer->SetMaterial(EnsureCellMaterialInstance());
	// См. одноимённый комментарий в RenderGridImmediate().
	CellsRenderer->SetScaleMultiplier(CellMeshScaleMultiplier);

	if (ShouldGhostShapeReplaceDetailedRender())
	{
		// Тот же принцип, что в RenderGridImmediate() - см. её doc-comment
		// у аналогичной ветки. bEnableChunkedRender здесь тоже не важен:
		// нет живых инстансов - нечего разливать по кадрам, а если реавил
		// с прошлого поколения ещё шёл, Render() с пустым списком инстансов
		// (через BeginRender()+полный слив внутри) сам обнуляет
		// PendingInstances/PendingCursor - bChunkedRenderInProgress
		// сама подхватит это на следующем Tick()/AdvanceChunkedRender().
		CellsRenderer->Render(*Grid, TArray<FCellRenderInstance>());
		// См. ту же ветку в RenderGridImmediate() - ноль здесь фактический.
		NoteRenderedCells(0);
		RenderSelectionOverlay();
		UE_LOG(LogTemp, Log, TEXT("RenderCurrentGrid: детальный рендер пропущен - Ghost Shape покрывает всю сетку целиком (%d живых клеток)"),
			Grid->Num());
		return;
	}

	TArray<FCellRenderInstance> Instances;
	BuildCellRenderData(Instances);
	// До BeginRender()/Render() ниже: там Instances уже перемещён. Значение -
	// это то, что уйдёт в AddInstances() целиком, даже если чанковый рендер
	// размажет его по кадрам: график про объём работы, а не про текущий кадр.
	NoteRenderedCells(LastRenderStats.RenderedCellCount);

	const FVector CameraLocation = (GamePC && GamePC->PlayerCameraManager)
		? GamePC->PlayerCameraManager->GetCameraLocation()
		: FVector::ZeroVector;

	if (bEnableChunkedRender)
	{
		CellsRenderer->BeginRender(*Grid, MoveTemp(Instances), ChunkedRenderOrder, CameraLocation);
	}
	else
	{
		CellsRenderer->Render(*Grid, MoveTemp(Instances));
	}

	// Подсветка выделения - всегда одним снимком (не чанкуется, выделение
	// всегда маленькое подмножество), не-op, если SelectedCells пуст.
	RenderSelectionOverlay();

	if (bEnableChunkedRender)
	{
		bChunkedRenderInProgress = true;
		ChunkedRenderStartSeconds = FPlatformTime::Seconds();
		ChunkedRenderFrameCount = 0;

		UE_LOG(LogTemp, Log, TEXT("RenderCurrentGrid: живых клеток %d - рендер разлит по кадрам"),
			Grid->Num());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("RenderCurrentGrid: живых клеток %d отрисовано"),
			Grid->Num());
		// Чанкинг выключен - всё уехало одним кадром, как в
		// RenderGridImmediate(). Это же и базовая линия "до чанкинга", с
		// которой сравнивается мс/кадр разлитого варианта.
		LogRenderTimings(TEXT("oneshot"), CellsRenderer->GetLastRenderTimings(),
			LastRenderStats.RenderedCellCount, 1);
	}
}

void AAutomataOrchestrator::SetChunkedRenderEnabled(bool bEnabled)
{
	bEnableChunkedRender = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("SetChunkedRenderEnabled: рендер по кадрам %s"), bEnabled ? TEXT("включён") : TEXT("выключен"));
}

void AAutomataOrchestrator::CycleChunkedRenderOrder()
{
	constexpr uint8 NumOrders = (uint8)EChunkedRenderOrder::FromCenterOutward + 1;
	ChunkedRenderOrder = (EChunkedRenderOrder)(((uint8)ChunkedRenderOrder + 1) % NumOrders);
	UE_LOG(LogTemp, Log, TEXT("CycleChunkedRenderOrder: порядок реавила -> %s"), *UEnum::GetValueAsString(ChunkedRenderOrder));
}

void AAutomataOrchestrator::SetWaitForChunkedRenderToFinish(bool bWait)
{
	bWaitForChunkedRenderToFinish = bWait;
	UE_LOG(LogTemp, Log, TEXT("SetWaitForChunkedRenderToFinish: режим ожидания разлива %s"), bWait ? TEXT("включён") : TEXT("выключен"));
}

void AAutomataOrchestrator::SetCellShadowsEnabled(bool bEnabled)
{
	bCellsCastShadows = bEnabled;
	bRenderPresetModified = true;
	UE_LOG(LogTemp, Log, TEXT("SetCellShadowsEnabled: тени от клеток %s"), bEnabled ? TEXT("включены") : TEXT("выключены"));

	// Применяем немедленно, не дожидаясь следующего рендера - SetCastShadow()
	// сама обновляет SceneProxy, ей не нужен новый AddInstances() (ровно та же
	// причина, по которой SetCellCullingEnabled() зовёт ApplyCellCullDistances()).
	ApplyCellShadowSettings();
}

void AAutomataOrchestrator::ApplyCellShadowSettings()
{
	// К ОБОИМ компонентам клеток, а не только к активному - если
	// CellMeshComponentType переключат позже, второй не должен остаться со
	// старой настройкой (то же соображение, что в ApplyCellCullDistances()).
	CellsMeshHierarchical->SetCastShadow(bCellsCastShadows);
	CellsMeshFlat->SetCastShadow(bCellsCastShadows);

	if (SelectionMeshComponent)
	{
		SelectionMeshComponent->SetCastShadow(bCellsCastShadows);
	}
}

const FName AAutomataOrchestrator::CellBorderWidthParameter(TEXT("BorderWidth"));

void AAutomataOrchestrator::SetCellBorderWidth(float NewBorderWidth)
{
	// Тот же зажим, что в meta у самого свойства: сеттер существует ради
	// слайдера HUD, а тот пишет значение напрямую и об ограничениях панели не
	// знает.
	CellBorderWidth = FMath::Clamp(NewBorderWidth, 0.0f, 0.25f);

	// Ни перерисовки, ни пересчёта поколения: значение уезжает в uniform-буфер
	// материала и видно уже на следующем кадре, даже на полностью
	// остановленной симуляции.
	EnsureCellMaterialInstance();
}

UMaterialInterface* AAutomataOrchestrator::EnsureCellMaterialInstance()
{
	if (!CellMaterial)
	{
		CellMaterialInstance = nullptr;
		return nullptr;
	}

	// Parent, а не GetBaseMaterial(): последний возвращает корневой UMaterial,
	// поэтому подмена CellMaterial на другой Material Instance того же родителя
	// осталась бы незамеченной, и клетки продолжили бы рисоваться прежним.
	if (!CellMaterialInstance || CellMaterialInstance->Parent != CellMaterial)
	{
		CellMaterialInstance = UMaterialInstanceDynamic::Create(CellMaterial, this);
		bCellBorderParameterWarned = false;
	}

	if (!CellMaterialInstance)
	{
		// Рисовать без канта лучше, чем не рисовать вовсе.
		UE_LOG(LogTemp, Warning, TEXT("EnsureCellMaterialInstance: не удалось создать динамический инстанс материала - ширина контура меняться не будет"));
		return CellMaterial;
	}

	// Отсутствующий параметр - тихий отказ: SetScalarParameterValue() в этом
	// случае просто ничего не делает, и снаружи это выглядит как сломанный
	// ползунок. Проверяем один раз на инстанс и говорим об этом вслух.
	if (!bCellBorderParameterWarned)
	{
		float ExistingValue = 0.0f;
		if (!CellMaterialInstance->GetScalarParameterValue(FMaterialParameterInfo(CellBorderWidthParameter), ExistingValue))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("EnsureCellMaterialInstance: в материале клеток нет скалярного параметра '%s' - CellBorderWidth ни на что не влияет"),
				*CellBorderWidthParameter.ToString());
		}
		bCellBorderParameterWarned = true;
	}

	CellMaterialInstance->SetScalarParameterValue(CellBorderWidthParameter, CellBorderWidth);
	return CellMaterialInstance;
}

void AAutomataOrchestrator::SetBackgroundVisible(bool bVisible)
{
	bShowBackground = bVisible;
	bRenderPresetModified = true;
	UE_LOG(LogTemp, Log, TEXT("SetBackgroundVisible: фон %s"), bVisible ? TEXT("показан") : TEXT("скрыт"));

	ApplyBackgroundVisibility();
}

void AAutomataOrchestrator::ApplyBackgroundVisibility()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Небо и облака НЕ прячем как актёров, а исключаем из основного прохода:
	// bRenderInMainPass выключает только отрисовку в кадр (basepass/прозрачность),
	// оставляя компонент в сцене для всего остального - в том числе для
	// real-time-захвата ASkyLight, который каждый кадр пересобирает кубмап
	// окружающего света ИМЕННО С НЕБА.
	//
	// Здесь и была ловушка. "Просто спрятать небо" гасит и свет, и это не
	// побочный эффект, а прямое следствие настройки уровня: у ASkyLight
	// bRealTimeCapture == true, и исчезнувшее небо оставляет захват без
	// источника - рассеянный свет уходит в ноль вместе с фоном. На замерах в
	// PIE (одна и та же точка камеры) пропадали синие и зелёные клетки, вся
	// картинка сваливалась в один тёплый направленный свет.
	//
	// Заморозка захвата (USkyLightComponent::SetRealTimeCaptureEnabled(false))
	// эту дыру НЕ закрывает - проверено там же и отвергнуто: она не
	// пересобирает кубмап на месте, а ставит пересъёмку в очередь
	// (SetCaptureIsDirty() внутри), и та всё равно отрабатывает по уже пустому
	// небу, замораживая чёрный кубмап. bRenderInMainPass сохраняет освещение
	// полностью. Источники света (ASkyLight/ADirectionalLight) не трогаются
	// вовсе - в этом и весь смысл.
	for (TActorIterator<ASkyAtmosphere> It(World); It; ++It)
	{
		if (USkyAtmosphereComponent* SkyComponent = It->GetComponent())
		{
			SkyComponent->SetRenderInMainPass(bShowBackground);
		}
	}
	for (TActorIterator<AVolumetricCloud> It(World); It; ++It)
	{
		// У AVolumetricCloud нет публичного геттера компонента (в отличие от
		// ASkyAtmosphere::GetComponent()), поэтому ищем по классу.
		if (UVolumetricCloudComponent* CloudComponent = It->FindComponentByClass<UVolumetricCloudComponent>())
		{
			CloudComponent->SetRenderInMainPass(bShowBackground);
		}
	}

	// У AExponentialHeightFog такого переключателя нет, поэтому туман прячем
	// целиком. Проверено в том же замере: на освещении это не сказывается -
	// туман, в отличие от неба, захвату ASkyLight светом не служит.
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		It->SetActorHiddenInGame(!bShowBackground);
	}
}

void AAutomataOrchestrator::RunRenderConsoleCommand(const FString& Command)
{
	// Через контроллер, а не GEngine->Exec(): команды VIEWMODE адресованы
	// вьюпорту конкретного локального игрока, и только этот путь их доставляет
	// (им же слал их прежний хоткей Lit/Unlit). Для r.* разницы нет, поэтому
	// весь список идёт одним путём, без ветвления по типу команды.
	if (GamePC)
	{
		GamePC->ConsoleCommand(Command, /*bWriteToLog=*/false);
		return;
	}

	// Контроллер ещё не готов (до BeginPlay) - r.* всё равно применятся, а
	// VIEWMODE в этот момент и применять некуда.
	if (GEngine)
	{
		GEngine->Exec(GetWorld(), *Command);
	}
}

TArray<FRenderPreset> AAutomataOrchestrator::GetRenderPresets() const
{
	return RenderPresets::GetAll();
}

FString AAutomataOrchestrator::GetActiveRenderPresetName() const
{
	const TArray<FRenderPreset>& Presets = RenderPresets::GetAll();
	return Presets.IsValidIndex(ActiveRenderPresetIndex) ? Presets[ActiveRenderPresetIndex].Name : FString();
}

void AAutomataOrchestrator::ApplyRenderPreset(int32 PresetIndex)
{
	const TArray<FRenderPreset>& Presets = RenderPresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyRenderPreset: нет профиля с индексом %d (всего %d) - ничего не меняем"), PresetIndex, Presets.Num());
		return;
	}

	const FRenderPreset& Preset = Presets[PresetIndex];

	// Движковые cvar'ы. Каждый профиль задаёт весь список целиком, поэтому
	// восстанавливать что-либо от предыдущего не нужно - см. doc-comment
	// FRenderPreset::ConsoleCommands.
	for (const FString& Command : Preset.ConsoleCommands)
	{
		RunRenderConsoleCommand(Command);
	}
	RunRenderConsoleCommand(Preset.bLit ? TEXT("VIEWMODE LIT") : TEXT("VIEWMODE UNLIT"));

	// Настройки клеток. Пишем поля напрямую, а не через сеттеры: каждый из них
	// сам дёргает применение и перерисовку, и пройти по ним подряд означало бы
	// три-четыре полных RenderGridImmediate() на одно нажатие клавиши. Ниже
	// всё применяется по разу.
	bCellsCastShadows = Preset.bCellsCastShadows;
	bEnableCellCulling = Preset.bCellCullingEnabled;
	CellCullStartDistance = Preset.CellCullStartDistance;
	CellCullEndDistance = Preset.CellCullEndDistance;
	bShowBackground = Preset.bShowBackground;

	ApplyCellShadowSettings();
	ApplyCellCullDistances();
	ApplyBackgroundVisibility();

	// Ghost Shape - последним и через сеттер: он единственный меняет САМ набор
	// рисуемых объектов (без куба отсечения силуэт заменяет поклеточный рендер
	// целиком), и его сеттер уже делает ровно то, что здесь нужно - перерисовать
	// текущее состояние и пересобрать силуэт, не дожидаясь нового поколения.
	SetGhostShapeEnabled(Preset.bGhostShapeEnabled);

	ActiveRenderPresetIndex = PresetIndex;
	// Строго после SetGhostShapeEnabled() и прочих сеттеров: каждый из них
	// поднимает этот флаг ("настройку профиля тронули руками"), и сбрасывать
	// его нужно уже по итогам всего применения.
	bRenderPresetModified = false;

	UE_LOG(LogTemp, Log, TEXT("ApplyRenderPreset: профиль рендера -> %s (%s)"), *Preset.Name, *Preset.Description);
}

void AAutomataOrchestrator::AdvanceChunkedRender()
{
	++ChunkedRenderFrameCount;

	// Бюджет ChunkedRenderCellsPerFrame уходит единственному рендереру
	// ЦЕЛИКОМ - прежнее деление между возрастными бакетами исчезло вместе с
	// ними (см. doc-comment AdvanceChunkedRender() в заголовке).
	if (CellsRenderer && CellsRenderer->AdvanceRenderChunk(ChunkedRenderCellsPerFrame))
	{
		return;
	}

	bChunkedRenderInProgress = false;

	const double TotalSeconds = FPlatformTime::Seconds() - ChunkedRenderStartSeconds;
	UE_LOG(LogTemp, Log, TEXT("AdvanceChunkedRender: рендер разлитый по кадрам завершён - живых клеток %d за %d кадр(ов)/%.2f мс"),
		Grid ? Grid->Num() : 0, ChunkedRenderFrameCount, TotalSeconds * 1000.0);

	// TotalSeconds выше - это стена от BeginRender() до последнего чанка, т.е.
	// в основном время самих кадров, а не работы рендера. Полезная для
	// подбора ChunkedRenderCellsPerFrame величина - только в разбивке ниже.
	if (CellsRenderer)
	{
		LogRenderTimings(TEXT("chunked"), CellsRenderer->GetLastRenderTimings(),
			LastRenderStats.RenderedCellCount, ChunkedRenderFrameCount);
	}
}

void AAutomataOrchestrator::FinishChunkedRenderImmediately()
{
	if (!bChunkedRenderInProgress)
	{
		return;
	}

	while (CellsRenderer && CellsRenderer->AdvanceRenderChunk(TNumericLimits<int32>::Max()))
	{
	}

	bChunkedRenderInProgress = false;

	UE_LOG(LogTemp, Log, TEXT("FinishChunkedRenderImmediately: чанковый рендер довершён одним разом (остановлен через Stop) - живых клеток %d"),
		Grid ? Grid->Num() : 0);
}
