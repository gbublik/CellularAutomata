// GPU-сессия прогона (bGpuResidentRun): сетка живёт на видеокарте между
// заходами, на CPU возвращается на паузе и при отложенных действиях. См.
// FGpuResidentSimulation и docs/compute-strategies.md.

#include "Orchestration/AutomataOrchestrator.h"
#include "Async/Async.h"
#include "Automata/Rendering/CellGpuInstancesComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "DynamicRHI.h"
#include "Engine/StaticMesh.h"
#include "RHIStats.h"

int64 AAutomataOrchestrator::ComputeGpuLiveCellLimit(bool bWithDecay) const
{
	double BudgetBytes = 0.0;
	if (GpuResidentMemoryBudgetMB > 0)
	{
		BudgetBytes = double(GpuResidentMemoryBudgetMB) * 1024.0 * 1024.0;
	}
	else
	{
		// 40% видеокарты: остальное - сцене, GPU-Scene с экземплярами клеток
		// и HISM, который вернётся на паузе. Размер неизвестен - 2 ГБ.
		FTextureMemoryStats MemoryStats;
		RHIGetTextureMemoryStats(MemoryStats);
		BudgetBytes = MemoryStats.DedicatedVideoMemory > 0
			? double(MemoryStats.DedicatedVideoMemory) * 0.4
			: 2.0 * 1024.0 * 1024.0 * 1024.0;
	}
	const int64 ByMemory = (int64)(BudgetBytes / FGpuResidentSimulation::BytesPerCell(bWithDecay));
	return FMath::Min<int64>(GpuVolumeCellLimit, ByMemory);
}

UCellGpuInstancesComponent* AAutomataOrchestrator::EnsureGpuCellsComponent()
{
	UInstancedStaticMeshComponent* Cells = GetActiveCellsMeshComponent();
	if (!Cells)
	{
		return nullptr;
	}

	// Под компонентом клеток и с единичным относительным трансформом: позиции
	// GPU-пути - те же GridToWorld(), что HISM получает как локальные.
	if (!GpuCellsComponent)
	{
		GpuCellsComponent = NewObject<UCellGpuInstancesComponent>(this, TEXT("GpuCells"), RF_Transient);
		GpuCellsComponent->SetupAttachment(Cells);
		GpuCellsComponent->RegisterComponent();
	}
	else if (GpuCellsComponent->GetAttachParent() != Cells)
	{
		GpuCellsComponent->AttachToComponent(Cells, FAttachmentTransformRules::KeepRelativeTransform);
		GpuCellsComponent->SetRelativeTransform(FTransform::Identity);
	}

	// Тот же меш, материал и тени, что у HISM (см. RenderCurrentGrid()).
	// Каждая смена пересоздаёт прокси, поэтому только при расхождении.
	if (GpuCellsComponent->GetStaticMesh() != CellMesh)
	{
		GpuCellsComponent->SetStaticMesh(CellMesh);
	}
	UMaterialInterface* Material = EnsureCellMaterialInstance();
	if (Material && GpuCellsComponent->GetMaterial(0) != Material)
	{
		GpuCellsComponent->SetMaterial(0, Material);
	}
	if (GpuCellsComponent->CastShadow != Cells->CastShadow)
	{
		GpuCellsComponent->SetCastShadow(Cells->CastShadow);
	}
	return GpuCellsComponent;
}

TSharedRef<const FCellGpuDrawParams, ESPMode::ThreadSafe> AAutomataOrchestrator::BuildGpuDrawParams()
{
	TSharedRef<FCellGpuDrawParams, ESPMode::ThreadSafe> Params = MakeShared<FCellGpuDrawParams, ESPMode::ThreadSafe>();

	if (Grid.IsValid())
	{
		const FLatticeTransform& Lattice = Grid->GetLattice();
		Params->Step = FVector3f(Lattice.CellWorldStep);
		Params->Origin = FVector3f(Lattice.Origin);

		// Масштаб - как FInstancedMeshCellGridRenderer::BeginRender(): ширина
		// меша по X подгоняется под шаг решётки, поверх - множитель.
		double Scale = 1.0;
		if (CellMesh)
		{
			const double MeshReferenceSize = CellMesh->GetBounds().BoxExtent.X * 2.0;
			if (!FMath::IsNearlyZero(MeshReferenceSize))
			{
				Scale = Lattice.GetPlanarCellSize() / MeshReferenceSize;
			}
		}
		Params->Scale = FVector3f((float)(Scale * CellMeshScaleMultiplier));
	}

	auto Pack = [](const TArray<FColor>& Lut, TArray<uint32>& Out)
	{
		Out.SetNumZeroed(256);
		for (int32 Index = 0; Index < FMath::Min(Lut.Num(), 256); ++Index)
		{
			Out[Index] = uint32(Lut[Index].R) | (uint32(Lut[Index].G) << 8) | (uint32(Lut[Index].B) << 16);
		}
	};
	TArray<FColor> Lut;
	BuildAgeColorLut(Lut, /*bSRGB=*/false);
	Pack(Lut, Params->AgeLut);
	Lut.Reset();
	if (States > 2)
	{
		BuildDecayColorLut(Lut, /*bSRGB=*/false);
	}
	Pack(Lut, Params->DecayLut);

	if (bRenderSurfaceOnly)
	{
		TArray<FIntVector> FaceOffsets;
		BuildFaceNeighborOffsets(FaceOffsets);
		for (const FIntVector& Offset : FaceOffsets)
		{
			Params->FaceOffsets.Add(FIntVector4(Offset.X, Offset.Y, Offset.Z, 0));
		}
	}

	// Тот же фильтр, что у BuildCellRenderData(), с запоминанием камеры среза.
	const CellVisibility::FFilter Filter = BuildVisibilityFilter(/*bUpdateSliceCameraState=*/true);
	Params->bAgeFilterActive = Filter.bAgeFilterActive;
	if (Filter.bAgeFilterActive)
	{
		for (int32 Age = 0; Age < FMath::Min(Filter.AgeMask.Num(), 256); ++Age)
		{
			if (Filter.AgeMask[Age])
			{
				Params->AgeMask[Age >> 5] |= 1u << (Age & 31);
			}
		}
	}
	Params->bSliceActive = Filter.bSliceActive;
	Params->SliceOrigin = FVector3f(Filter.SliceOrigin);
	Params->SliceForward = FVector3f(Filter.SliceForward);
	Params->SliceMinDepth = (float)Filter.SliceMinDepth;
	Params->SliceMaxDepth = (float)Filter.SliceMaxDepth;

	FBox CullBounds;
	if (GetActiveCullBounds(CullBounds))
	{
		Params->bCullActive = true;
		Params->CullMin = FVector3f(CullBounds.Min);
		Params->CullMax = FVector3f(CullBounds.Max);
	}

	return Params;
}

void AAutomataOrchestrator::ShowGpuLiveCells(const FGpuLiveDispatchResult& Result)
{
	if (!GpuLive || !Result.DrawParams.IsValid())
	{
		return;
	}
	UCellGpuInstancesComponent* Comp = EnsureGpuCellsComponent();
	if (!Comp)
	{
		return;
	}

	// Границы примитива - объём сессии с полями в клетку.
	const FCellGpuDrawParams& Params = *Result.DrawParams;
	const FVector Step(Params.Step);
	const FVector Origin(Params.Origin);
	const FVector A = FVector(Result.VolumeMin) * Step + Origin - Step;
	const FVector B = FVector(Result.VolumeMin + Result.VolumeDim) * Step + Origin + Step;
	const FBox Bounds(A.ComponentMin(B), A.ComponentMax(B));

	Comp->ShowResidentCells(GpuLive->GetGpuPlanes(), Result.DrawParams.ToSharedRef(), Result.Stats.ExtraCount, Bounds,
		GetActiveCellsMeshComponent());

	// Те же числа, что HUD берёт у HISM-пути; выгрузки через CPU нет вовсе.
	LastRenderStats.RenderedCellCount = (int32)FMath::Min<int64>(Result.Stats.ExtraCount, MAX_int32);
	LastRenderStats.TotalCellCount = (int32)FMath::Min<int64>(Result.Stats.AliveCount, MAX_int32);
	LastRenderStats.EstimatedUploadMB = 0.0;
}

bool AAutomataOrchestrator::CanUseGpuLive() const
{
	// Только Play: автошаг по F и ручной шаг живут своим ритмом и остаются на
	// прежнем пути. Исключения - режимы, которым CPU-сетка нужна на каждом
	// поколении: автоперебор (проверки вымирания/застоя/взрыва), серийная
	// съёмка (растеризует сетку) и Ghost Shape (строит меш из сетки).
	return bGpuResidentRun
		&& ComputeMethod == EComputeMethod::Gpu
		&& bSimulationRunning
		&& !bFastStepActive
		&& !bGpuLiveRefused
		&& !bAutoReseedOnExtinction
		&& !bSeriesCaptureActive
		&& !bEnableGhostShape
		&& Grid.IsValid();
}

int64 AAutomataOrchestrator::GetAliveCountForStats() const
{
	if (bGpuLiveActive)
	{
		return GpuLiveAliveCount;
	}
	return Grid.IsValid() ? Grid->Num() : 0;
}

bool AAutomataOrchestrator::BeginGpuLive()
{
	if (bGpuLiveActive)
	{
		return true;
	}
	if (bStepInProgress || !Grid.IsValid())
	{
		return false;
	}

	// Дорисовка чанкового разлива читает Grid - доводим её сейчас, пока сетка
	// ещё на CPU в последнем виде (дальше она будет "старой" всю сессию).
	FinishChunkedRenderImmediately();

	const FCellularAutomatonRule Rule = BuildRule();
	const int32 Steps = FMath::Max(1, StepsPerRender);
	// Запас вокруг структуры - на два захода роста: один заход занимает, ещё
	// один остаётся, чтобы раздвигание не случалось на каждом заходе.
	const int32 MinHalo = FMath::Max(1, Rule.GetNeighborExtent()) * Steps * 2;

	GpuLiveCellLimit = ComputeGpuLiveCellLimit(Rule.HasDecayStates());

	TUniquePtr<FGpuResidentSimulation> Session = MakeUnique<FGpuResidentSimulation>();
	FString Error;
	if (!Session->Upload(*Grid, Rule, MinHalo, GpuLiveCellLimit, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("GPU-сессия не открыта: %s. Прогон идёт прежним путём до следующего Start()."), *Error);
		bGpuLiveRefused = true;
		return false;
	}

	GpuLive = MoveTemp(Session);
	GpuLiveAliveCount = GpuLive->GetStats().AliveCount;
	bGpuLiveActive = true;
	bGpuLiveDispatchInFlight = false;
	// Вся сессия для остального кода - один длинный шаг (см. doc-comment GpuLive).
	bStepInProgress = true;

	UE_LOG(LogTemp, Log, TEXT("GPU-сессия открыта: %lld живых, поколений за заход %d, потолок объёма %lld клеток (%.0f МБ)"),
		GpuLiveAliveCount, Steps, GpuLiveCellLimit,
		double(GpuLiveCellLimit) * FGpuResidentSimulation::BytesPerCell(Rule.HasDecayStates()) / (1024.0 * 1024.0));
	return true;
}

void AAutomataOrchestrator::DispatchGpuLive()
{
	if (!bGpuLiveActive || bGpuLiveDispatchInFlight || !GpuLive)
	{
		return;
	}

	// Правило и лимит - снимок UPROPERTY на игровом потоке, как в StepAsync().
	FCellularAutomatonRule Rule = BuildRule();
	const int32 Steps = FMath::Max(1, StepsPerRender);
	const int64 VolumeLimit = GpuLiveCellLimit;

	// Видимые считаются в том же заходе, по настройкам этого кадра, - число
	// приезжает со сводкой и задаёт ёмкость экземпляров до записи.
	TSharedRef<const FCellGpuDrawParams, ESPMode::ThreadSafe> DrawParams = BuildGpuDrawParams();
	FGpuResidentSimulation::FExtraPass CountVisible =
		[DrawParams](FRDGBuilder& GraphBuilder, const FGpuResidentPlanes& Planes, FRDGBuffer* Bits, FRDGBuffer* Ages, FRDGBuffer* Decay, FRDGBufferUAV* StatsUAV)
		{
			CellGpuDrawing::AddCountVisiblePass(GraphBuilder, *DrawParams, Planes, Bits, Ages, Decay, StatsUAV,
				(uint32)FGpuResidentSimulation::ExtraStatsWord);
		};

	bGpuLiveDispatchInFlight = true;
	GpuLiveResult = MakeShared<FGpuLiveDispatchResult, ESPMode::ThreadSafe>();
	GpuLiveResult->DrawParams = DrawParams;

	FGpuResidentSimulation* Session = GpuLive.Get();
	TSharedPtr<FGpuLiveDispatchResult, ESPMode::ThreadSafe> Result = GpuLiveResult;
	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	// Session - сырой указатель: сессию разрушают только после
	// PendingStepFuture.Wait() (EndGpuLive(), EndPlay()).
	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[Session, Rule = MoveTemp(Rule), Steps, VolumeLimit, Result, WeakThis, CountVisible = MoveTemp(CountVisible)]()
		{
			const double StartSeconds = FPlatformTime::Seconds();
			FString Error;
			const int32 Done = Session->Advance(Rule, Steps, VolumeLimit, Error, CountVisible);

			Result->Generations = Done;
			Result->Stats = Session->GetStats();
			Result->VolumeMin = Session->GetMinCell();
			Result->VolumeDim = Session->GetVolumeDim();
			Result->Seconds = FPlatformTime::Seconds() - StartSeconds;
			Result->Error = MoveTemp(Error);

			AsyncTask(ENamedThreads::GameThread, [WeakThis]()
			{
				if (AAutomataOrchestrator* StrongThis = WeakThis.Get())
				{
					StrongThis->ApplyGpuLiveResult();
				}
			});
		});
}

void AAutomataOrchestrator::ConsumeGpuLiveResult(FGpuLiveDispatchResult& Result)
{
	if (Result.bConsumed)
	{
		return;
	}
	Result.bConsumed = true;

	if (Result.Generations <= 0)
	{
		return;
	}

	// Те же счётчики, что CommitComputedGenerations() ведёт для обычного шага:
	// темп следующих заходов, HUD, точка графика. Сетка на CPU при этом не
	// трогается - она вернётся при закрытии сессии.
	LastDispatchGenerations = Result.Generations;
	GpuLiveAliveCount = Result.Stats.AliveCount;
	GenerationCount += Result.Generations;
	EditRedoStack.Reset();
	SelectedCells.Reset();
	AppendGenerationSample();
}

void AAutomataOrchestrator::ApplyGpuLiveResult()
{
	TSharedPtr<FGpuLiveDispatchResult, ESPMode::ThreadSafe> Result = GpuLiveResult;
	bGpuLiveDispatchInFlight = false;

	// Сессию закрыли, пока заход был в полёте: EndGpuLive() уже учёл его итог.
	if (!bGpuLiveActive || !Result.IsValid())
	{
		return;
	}

	if (Result->Generations <= 0)
	{
		// Объём не раздвинуть под лимит, либо правило сменило режим угасания.
		// Закрываем сессию; следующий тик откроет её заново (режим угасания)
		// или пойдёт прежним путём (лимит - см. bGpuLiveRefused).
		UE_LOG(LogTemp, Warning, TEXT("GPU-сессия: заход не выполнен (%s) - сессия закрыта"), *Result->Error);
		if (Result->Error.Contains(TEXT("GpuVolumeCellLimit")))
		{
			bGpuLiveRefused = true;
		}
		EndGpuLive(true);
		return;
	}

	ConsumeGpuLiveResult(*Result);

	// R, N или Ctrl+Z, нажатые во время сессии, ждут CPU-сетки - закрываем
	// сессию, и они отработают в EndGpuLive() обычным путём. Если прогон
	// продолжается, следующий тик откроет сессию заново.
	if (bResetToInitialStatePending || bNewSeedPending || bStepBackwardPending)
	{
		EndGpuLive(true);
		return;
	}

	ShowGpuLiveCells(*Result);
}

void AAutomataOrchestrator::EndGpuLive(bool bRenderAfter)
{
	if (!bGpuLiveActive)
	{
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();

	// Заход в полёте держит сырой указатель на сессию - дождаться его.
	if (PendingStepFuture.IsValid())
	{
		PendingStepFuture.Wait();
	}
	// Его итог мог ещё не дойти до игрового потока - учесть сейчас, иначе
	// поколения, уже лежащие в читаемой назад сетке, не попали бы в счётчик.
	if (GpuLiveResult.IsValid())
	{
		ConsumeGpuLiveResult(*GpuLiveResult);
	}

	TUniquePtr<FCellGrid> NewGrid = CreateGrid();
	GpuLive->Download(*NewGrid);

	UE_LOG(LogTemp, Log, TEXT("GPU-сессия закрыта: сетка на CPU, %d живых, раздвиганий объёма %d (%.1f мс)"),
		NewGrid->Num(), GpuLive->GetRegrowCount(), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);

	GpuLive.Reset();
	GpuLiveResult.Reset();
	bGpuLiveActive = false;
	bGpuLiveDispatchInFlight = false;

	// GPU-картинку убрать (слоты GPU-Scene освобождаются под HISM) и вернуть
	// HISM на экран - его тут же перерисует RenderCurrentGrid() или
	// отложенное действие.
	if (GpuCellsComponent)
	{
		GpuCellsComponent->ClearCells();
	}

	Grid = MoveTemp(NewGrid);
	SelectedCells.Reset();
	bStepInProgress = false;

	// Отложенное действие само перестроит и перерисует сетку.
	if (RunPendingGridActions())
	{
		return;
	}

	if (bRenderAfter)
	{
		RenderCurrentGrid();
	}
}
