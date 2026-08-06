// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/CellAging.h"
#include "Automata/Simulation/CellDecay.h"
#include "Automata/Simulation/ComputeStrategy/CellularAutomatonComputeStrategy.h"
#include "Async/Async.h"


void AAutomataOrchestrator::DeleteSelectedCells()
{
	// Мутируем Grid - фоновый шаг (Next()/StepAsync()) в этот момент его
	// читает, тот же guard, что у всех путей изменения сетки.
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("DeleteSelectedCells: фоновый шаг ещё считается - подождите его завершения"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("DeleteSelectedCells: сетка не инициализирована"));
		return;
	}

	if (SelectedCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("DeleteSelectedCells: нет выделенных клеток - сначала выделите что-нибудь мышкой в режиме выделения (Tab)"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("DeleteSelectedCells: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	// Снимаем прежнее состояние клеток ДО правки - это и есть то, что вернёт
	// Ctrl+Z (см. RecordEdit()/UndoLastAction()). Заодно запись отсеивает
	// невыделенные-но-мёртвые клетки, так что она же и считает удалённые.
	FCellEditRecord Record = CellEditJournal::MakeDeleteRecord(*Grid, SelectedCells, GenerationCount);
	const int32 KilledCount = Record.Edits.Num();

	CellEditJournal::ApplyForward(*Grid, Record);
	RecordEdit(MoveTemp(Record));

	SelectedCells.Reset();
	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("DeleteSelectedCells: удалено %d клеток, живых осталось %d"), KilledCount, Grid->Num());
}

void AAutomataOrchestrator::RecordEdit(FCellEditRecord&& Record)
{
	if (Record.Edits.Num() == 0)
	{
		// Правка никого не задела (выделение по пустоте) - не действие, и в
		// стеке отмены ему делать нечего: Ctrl+Z, снимающий "ничего", выглядел
		// бы как несработавший.
		return;
	}

	// Любое новое действие обесценивает повтор - см. doc-comment EditRedoStack.
	EditRedoStack.Reset();

	const int64 NewTotal = CellEditJournal::TotalCells(EditJournal) + Record.Edits.Num();
	if (EditJournalMaxCells > 0 && NewTotal > EditJournalMaxCells)
	{
		// Старые записи не выбрасываем - журнал это сценарий, дыра в его
		// середине сделала бы пересчёт неверным молча (см. doc-comment
		// EditJournalMaxCells). Вместо этого честно признаём его неполным.
		bEditJournalOverflowed = true;
		EditJournal.Reset();
		ShowStatusMessage(StatusKey_StepBackward,
			TEXT("Журнал правок переполнен - отмена и шаг назад недоступны до сброса (R)"));
		UE_LOG(LogTemp, Warning, TEXT("RecordEdit: журнал правок перерос потолок в %lld клеток - отмена отключена до сброса"), EditJournalMaxCells);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("RecordEdit: %s на поколении %lld (в журнале %d записей)"),
		*Record.Description, Record.Generation, EditJournal.Num() + 1);

	EditJournal.Add(MoveTemp(Record));
}

void AAutomataOrchestrator::ResetToInitialState()
{
	if (InitialStateCells.Num() == 0)
	{
		// StartFromSelection() ещё ни разу не вызывался в этой сессии (и
		// файл не загружался) - нет сохранённой точки возврата, поэтому
		// строим заново тем же генератором, что и старт. На практике сюда не
		// попадают: BeginPlay() зовёт GenerateState(), а тот заполняет
		// InitialStateCells - ветка защитная.
		GenerateState();
		return;
	}

	if (bStepInProgress)
	{
		// Не просто отказываем - откладываем до момента, когда фоновый шаг
		// сам применит свой результат (ApplyStepResult()/завершение Next()),
		// оба проверяют этот флаг сразу после сброса bStepInProgress и сами
		// вызовут ResetToInitialState() ещё раз. Раньше здесь был только
		// warning-лог и return без взведения флага - нажатие R, совпавшее с
		// фоновым шагом, терялось молча, симуляция просто продолжала идти
		// дальше без сброса (см. doc-comment bResetToInitialStatePending).
		bResetToInitialStatePending = true;
		// Взаимоисключающ с отложенным рероллом: R после N означает "верни
		// исходный узор", а не "сначала перекати сид" (см. bNewSeedPending).
		bNewSeedPending = false;
		UE_LOG(LogTemp, Warning, TEXT("ResetToInitialState: фоновый шаг StepAsync() ещё считается - сброс отложен до его завершения"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("ResetToInitialState: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("ResetToInitialState: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	// Новый прогон убирает запечённый меш-снимок, если он был (см.
	// BakeCellsToMesh()) - как и в GenerateRandom().
	ClearBakedMesh();
	ClearGhostShape();

	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	SelectedCells.Reset();
	ResetGenerationCounter();

	for (const FIntVector& Cell : InitialStateCells)
	{
		Grid->SetAlive(Cell, true);
		Grid->SetAge(Cell, 0);
	}

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("ResetToInitialState: сетка восстановлена из сохранённой точки возврата (%d клеток)"), Grid->Num());
}

void AAutomataOrchestrator::UndoLastAction()
{
	// Правка мутирует Grid, а откат поколения его подменяет - оба пути и так
	// проверяют bStepInProgress сами, но у отмены правки нет отложенного пути
	// (в отличие от StepBackward()), поэтому проверяем здесь.
	if (bStepInProgress && EditJournal.Num() > 0 && EditJournal.Last().Generation == GenerationCount)
	{
		UE_LOG(LogTemp, Warning, TEXT("UndoLastAction: фоновый шаг ещё считается - подождите его завершения"));
		return;
	}

	// Последней была ручная правка ровно тогда, когда после неё не шагали:
	// её поколение совпадает с текущим. Если шагнули - последним действием был
	// шаг, и отменять надо его (см. doc-comment функции).
	if (EditJournal.Num() > 0 && EditJournal.Last().Generation == GenerationCount)
	{
		if (!Grid)
		{
			UE_LOG(LogTemp, Warning, TEXT("UndoLastAction: сетка не инициализирована"));
			return;
		}

		FCellEditRecord Record = EditJournal.Pop();
		CellEditJournal::ApplyInverse(*Grid, Record);

		// Выделение после отмены бессмысленно ровно как после самой правки:
		// клетки под ним изменились.
		SelectedCells.Reset();
		RenderGridImmediate();

		ShowStatusMessage(StatusKey_StepBackward,
			FString::Printf(TEXT("Отменено: %s"), *Record.Description));
		UE_LOG(LogTemp, Log, TEXT("UndoLastAction: отменено '%s', живых клеток %d"),
			*Record.Description, Grid->Num());

		// В повтор кладём ПОСЛЕ применения - Record до этого момента ещё нужен.
		EditRedoStack.Add(MoveTemp(Record));
		return;
	}

	// Отменять правку нечего - значит последним был шаг симуляции.
	StepBackward();
}

void AAutomataOrchestrator::RedoLastEdit()
{
	if (EditRedoStack.Num() == 0)
	{
		ShowStatusMessage(StatusKey_StepBackward, TEXT("Повторять нечего"));
		return;
	}

	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("RedoLastEdit: фоновый шаг ещё считается - подождите его завершения"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("RedoLastEdit: сетка не инициализирована"));
		return;
	}

	FCellEditRecord Record = EditRedoStack.Pop();

	// Поколение записи обязано совпасть с текущим: стек повтора гасится любым
	// новым действием (см. doc-comment EditRedoStack), так что разойтись они
	// могут только если какой-то путь забыл его погасить - тогда правка
	// накатилась бы не в ту точку траектории, и молчать об этом нельзя.
	if (Record.Generation != GenerationCount)
	{
		UE_LOG(LogTemp, Warning, TEXT("RedoLastEdit: правка сделана на поколении %lld, а сейчас %lld - повтор отменён"),
			Record.Generation, GenerationCount);
		EditRedoStack.Reset();
		return;
	}

	CellEditJournal::ApplyForward(*Grid, Record);
	SelectedCells.Reset();
	RenderGridImmediate();

	ShowStatusMessage(StatusKey_StepBackward,
		FString::Printf(TEXT("Повторено: %s"), *Record.Description));
	UE_LOG(LogTemp, Log, TEXT("RedoLastEdit: повторено '%s', живых клеток %d"),
		*Record.Description, Grid->Num());

	// Обратно в журнал - правка снова часть траектории.
	EditJournal.Add(MoveTemp(Record));
}

void AAutomataOrchestrator::StepBackward()
{
	if (bStepInProgress)
	{
		// Откладываем, а не отказываем - как R и N (см. doc-comment
		// bStepBackwardPending). Оба флага гасим: последнее нажатие выигрывает.
		bStepBackwardPending = true;
		bResetToInitialStatePending = false;
		bNewSeedPending = false;
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: фоновый шаг ещё считается - шаг назад отложен до его завершения"));
		return;
	}

	if (GenerationCount <= 0)
	{
		ShowStatusMessage(StatusKey_StepBackward, TEXT("Шаг назад: уже на поколении 0"));
		return;
	}

	if (InitialStateCells.Num() == 0)
	{
		// Без точки возврата пересчитывать не от чего. На практике недостижимо -
		// BeginPlay() зовёт GenerateState(), а тот заполняет InitialStateCells.
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: изначальный узор не сохранён - откатывать не от чего"));
		return;
	}

	if (bEditJournalOverflowed)
	{
		// Журнал правок перерос потолок и был сброшен - значит траектория
		// больше не воспроизводима, и пересчёт молча показал бы ход событий,
		// в котором части правок никогда не было. Лучше отказать (см.
		// doc-comment EditJournalMaxCells).
		ShowStatusMessage(StatusKey_StepBackward,
			TEXT("Шаг назад недоступен: журнал правок переполнен, траектория невоспроизводима (R - сброс)"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("StepBackward: активный CellsMesh-компонент отсутствует"));
		return;
	}

	// Непрерывный прогон и откат несовместимы: Tick() запустил бы следующий
	// StepAsync() сразу после того, как пересчёт вернёт предыдущее поколение, и
	// нажатие выглядело бы несработавшим (сетка мигнула бы назад и тут же ушла
	// вперёд). Останавливаем прогон, а не ставим на паузу - Pause() в этом
	// проекте про управление камерой, симуляцию останавливает Stop().
	if (bSimulationRunning)
	{
		Stop();
	}

	// Автошаг по удержанию Shift+F - второй потребитель той же ветки Tick() и
	// ровно та же проблема: он не гейтится bSimulationRunning, так что одного
	// Stop() выше недостаточно.
	if (bFastStepActive)
	{
		StopFastStep();
	}

	const int64 TargetGeneration = GenerationCount - 1;

	// Новый прогон убирает запечённый меш-снимок и призрачную оболочку - как
	// ResetToInitialState() и GenerateState().
	ClearBakedMesh();
	ClearGhostShape();

	// Засев строим ЗДЕСЬ, на игровом потоке (CreateGrid() читает живые
	// UPROPERTY), и отдаём его в фон по значению - Grid при этом не трогаем
	// вовсе: пока идёт пересчёт, на экране остаётся текущее поколение, а
	// подменится оно разом в продолжении. Тем же самым это отличается от
	// ResetToInitialState(), который рисует изначальный узор немедленно.
	TUniquePtr<FCellGrid> SeedGrid = CreateGrid();
	for (const FIntVector& Cell : InitialStateCells)
	{
		SeedGrid->SetAlive(Cell, true);
		SeedGrid->SetAge(Cell, 0);
	}

	// Ручные правки - вторая половина описания траектории (первая -
	// InitialStateCells выше): пересчёт обязан пройти через них, иначе он
	// вернёт ход событий, в котором правок не было. Берём только те, что
	// относятся к целевому отрезку; журнал упорядочен по поколению, так что
	// порядок сохраняется сам.
	TArray<FCellEditRecord> ReplayRecords;
	for (const FCellEditRecord& Record : EditJournal)
	{
		if (Record.Generation <= TargetGeneration)
		{
			ReplayRecords.Add(Record);
		}
	}

	// Поколение 0 - это изначальный узор плюс правки, сделанные до первого
	// шага: считать нечего, и в фон уходить незачем. Отдельная ветка, а не
	// делегирование в ResetToInitialState(), именно из-за правок - та строит
	// чистый узор и сбрасывает счётчик (а с ним и журнал).
	if (TargetGeneration == 0)
	{
		for (const FCellEditRecord& Record : ReplayRecords)
		{
			CellEditJournal::ApplyForward(*SeedGrid, Record);
		}

		Grid = MoveTemp(SeedGrid);
		SelectedCells.Reset();
		StepsSinceLastRender = 0;
		GenerationCount = 0;
		CellEditJournal::TrimAfter(EditJournal, 0);
		EditRedoStack.Reset();
		GenerationHistory::TrimAfter(GenerationSamples, 0);
		RenderGridImmediate();

		ShowStatusMessage(StatusKey_StepBackward, TEXT("Шаг назад: поколение 0 (изначальный узор)"));
		UE_LOG(LogTemp, Log, TEXT("StepBackward: поколение 0, живых клеток %d"), Grid->Num());
		return;
	}

	// Правило и стратегия - заново, как везде в проекте (см. Next()); геометрия
	// решётки и ChunkSize снимаются здесь, потому что промежуточные буферы
	// создаются уже в фоне, а живые UPROPERTY фоновому потоку читать нельзя.
	FCellularAutomatonRule AutomatonRule = BuildRule();
	TUniquePtr<FCellularAutomatonComputeStrategy> ComputeStrategy = CreateComputeStrategy();
	const FLatticeTransform LatticeSnapshot = BuildLatticeTransform();
	const int32 ChunkSizeSnapshot = ChunkSize;

	bStepInProgress = true;

	ShowStatusMessage(StatusKey_StepBackward,
		FString::Printf(TEXT("Шаг назад: пересчёт %lld поколений с нуля..."), TargetGeneration));

	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[AutomatonRule = MoveTemp(AutomatonRule), ComputeStrategy = MoveTemp(ComputeStrategy),
		 SeedGrid = MoveTemp(SeedGrid), ReplayRecords = MoveTemp(ReplayRecords), WeakThis,
		 TargetGeneration, LatticeSnapshot, ChunkSizeSnapshot]() mutable
		{
			const double StepStartSeconds = FPlatformTime::Seconds();

			// Тот же цикл, что в Next(), с одним отличием: источником владеет
			// сама лямбда (никакого сырого указателя на живой Grid - здесь его
			// и не нужно, пересчёт идёт от собственного засева), поэтому
			// предыдущее поколение освобождается сразу после того, как из него
			// посчитано следующее.
			TUniquePtr<FCellGrid> ResultGrid = MoveTemp(SeedGrid);

			// Правки, сделанные ДО первого шага, ложатся прямо на засев.
			int32 NextRecord = 0;
			while (NextRecord < ReplayRecords.Num() && ReplayRecords[NextRecord].Generation <= 0)
			{
				CellEditJournal::ApplyForward(*ResultGrid, ReplayRecords[NextRecord]);
				++NextRecord;
			}

			int64 StepsDone = 0;
			while (StepsDone < TargetGeneration)
			{
				TUniquePtr<FCellGrid> NextGrid = MakeUnique<FDenseCellGrid>(LatticeSnapshot, ChunkSizeSnapshot, AutomatonRule.HasDecayStates());

				// Через поколение, на котором была ручная правка,
				// ПЕРЕПРЫГИВАТЬ НЕЛЬЗЯ: GPU-стратегия считает пачку целиком
				// внутри себя, промежуточных поколений в пачке не существует,
				// и наложить правку было бы уже некуда. Поэтому пачка режется
				// по ближайшей записи журнала - на отрезках без правок она
				// остаётся полной, а рядом с правкой сама сжимается до шага.
				int64 MaxSteps = TargetGeneration - StepsDone;
				if (NextRecord < ReplayRecords.Num())
				{
					MaxSteps = FMath::Min(MaxSteps, ReplayRecords[NextRecord].Generation - StepsDone);
				}

				const int32 StepsRequested = static_cast<int32>(FMath::Clamp<int64>(MaxSteps, 1, MAX_int32));
				const int32 StepsAdvanced = ComputeStrategy->StepBatch(*ResultGrid, *NextGrid, AutomatonRule, StepsRequested);

				// Стратегия, продвинувшая больше одного поколения, обязана была
				// заполнить возрасты и угасание сама - см. Next().
				if (StepsAdvanced <= 1)
				{
					CellAging::ComputeAges(ResultGrid.Get(), *NextGrid);
					CellDecay::AdvanceDecayStates(ResultGrid.Get(), *NextGrid, AutomatonRule.GetStates());
				}

				ResultGrid = MoveTemp(NextGrid);
				StepsDone += FMath::Max(1, StepsAdvanced);

				// Правки этого поколения - сразу после того, как оно посчитано,
				// то есть ровно в том порядке, в каком всё происходило вживую.
				while (NextRecord < ReplayRecords.Num() && ReplayRecords[NextRecord].Generation <= StepsDone)
				{
					CellEditJournal::ApplyForward(*ResultGrid, ReplayRecords[NextRecord]);
					++NextRecord;
				}
			}

			const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;
			const int64 ComputeUploadBytes = ComputeStrategy->GetLastComputeUploadBytes();

			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, ResultGrid = MoveTemp(ResultGrid), StepSeconds, TargetGeneration, ComputeUploadBytes]() mutable
			{
				AAutomataOrchestrator* StrongThis = WeakThis.Get();
				if (!StrongThis)
				{
					return;
				}

				StrongThis->Grid = MoveTemp(ResultGrid);
				StrongThis->SelectedCells.Reset();
				StrongThis->bStepInProgress = false;

				// R и N, нажатые пока шёл пересчёт, важнее его результата - оба
				// всё равно перестроят сетку с нуля (см. ApplyStepResult()).
				if (StrongThis->bResetToInitialStatePending)
				{
					StrongThis->bResetToInitialStatePending = false;
					StrongThis->ResetToInitialState();
					return;
				}

				if (StrongThis->bNewSeedPending)
				{
					StrongThis->bNewSeedPending = false;
					StrongThis->NewSeed();
					return;
				}

				// Счётчик выставляется, а не уменьшается: сетка теперь ровно то,
				// что даёт TargetGeneration шагов от изначального узора.
				StrongThis->GenerationCount = TargetGeneration;
				StrongThis->LastGpuComputeUploadBytes = ComputeUploadBytes;
				StrongThis->StepsSinceLastRender = 0;

				// График теряет только хвост после точки отката - история ДО неё
				// верна и переживает откат (см. GenerationHistory::TrimAfter()).
				GenerationHistory::TrimAfter(StrongThis->GenerationSamples, TargetGeneration);

				// То же самое с журналом правок, и по более жёсткой причине: он
				// описывает траекторию, а правка, сделанная на поколении, до
				// которого мы больше не доходим, к ней не относится - оставь её,
				// и следующий откат воспроизвёл бы её заново.
				CellEditJournal::TrimAfter(StrongThis->EditJournal, TargetGeneration);

				// Повтор гасится любым новым действием - откат тоже действие
				// (см. doc-comment EditRedoStack).
				StrongThis->EditRedoStack.Reset();

				// Ещё один Ctrl+Z, нажатый пока считался этот - уходим в
				// следующий откат, не рисуя промежуточный кадр (он всё равно был
				// бы тут же заменён). Счётчик уже выставлен, так что новый
				// StepBackward() отсчитает от него.
				if (StrongThis->bStepBackwardPending)
				{
					StrongThis->bStepBackwardPending = false;
					StrongThis->StepBackward();
					return;
				}

				// Оболочка пересчитывается сразу, без своего интервала: она
				// описывает текущее поколение, а оно только что сменилось на
				// другое - причём назад, чего интервал не ожидает.
				if (StrongThis->bEnableGhostShape)
				{
					StrongThis->GhostShapeGenerationsSinceRefresh = 0;
					StrongThis->RefreshGhostShape();
				}

				// Немедленно и целиком, как ручной шаг: откат - осознанное
				// одиночное действие, размазывать его по кадрам незачем.
				StrongThis->RenderGridImmediate();

				StrongThis->ShowStatusMessage(StatusKey_StepBackward,
					FString::Printf(TEXT("Шаг назад: поколение %lld (пересчёт занял %.2f с)"),
						TargetGeneration, StepSeconds));

				UE_LOG(LogTemp, Log, TEXT("StepBackward: поколение %lld, живых клеток %d (пересчёт %lld поколений: %.2f мс [фоновый поток])"),
					TargetGeneration, StrongThis->Grid->Num(), TargetGeneration, StepSeconds * 1000.0);
			});
		});
}
