// GPU-сессия прогона (bGpuResidentRun): сетка живёт на видеокарте между
// заходами, на CPU возвращается на паузе и при отложенных действиях. См.
// FGpuResidentSimulation и docs/compute-strategies.md.

#include "Orchestration/AutomataOrchestrator.h"
#include "Async/Async.h"

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

	TUniquePtr<FGpuResidentSimulation> Session = MakeUnique<FGpuResidentSimulation>();
	FString Error;
	if (!Session->Upload(*Grid, Rule, MinHalo, GpuVolumeCellLimit, Error))
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

	UE_LOG(LogTemp, Log, TEXT("GPU-сессия открыта: %lld живых, поколений за заход %d"), GpuLiveAliveCount, Steps);
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
	const int64 VolumeLimit = GpuVolumeCellLimit;

	bGpuLiveDispatchInFlight = true;
	GpuLiveResult = MakeShared<FGpuLiveDispatchResult, ESPMode::ThreadSafe>();

	FGpuResidentSimulation* Session = GpuLive.Get();
	TSharedPtr<FGpuLiveDispatchResult, ESPMode::ThreadSafe> Result = GpuLiveResult;
	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	// Session - сырой указатель: сессию разрушают только после
	// PendingStepFuture.Wait() (EndGpuLive(), EndPlay()).
	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[Session, Rule = MoveTemp(Rule), Steps, VolumeLimit, Result, WeakThis]()
		{
			const double StartSeconds = FPlatformTime::Seconds();
			FString Error;
			const int32 Done = Session->Advance(Rule, Steps, VolumeLimit, Error);

			Result->Generations = Done;
			Result->Stats = Session->GetStats();
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
	}
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
