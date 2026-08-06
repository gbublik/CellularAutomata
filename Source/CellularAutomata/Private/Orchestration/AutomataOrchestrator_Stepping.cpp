// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/CellAging.h"
#include "Automata/Simulation/CellDecay.h"
#include "Automata/Simulation/ComputeStrategy/CellularAutomatonComputeStrategy.h"
#include "Async/Async.h"


void AAutomataOrchestrator::NewSeed()
{
	if (bStepInProgress)
	{
		// Не отказываем молча (GenerateState() ниже всё равно откажется -
		// фоновый поток читает *Grid), а откладываем до завершения шага, как
		// это делает R - см. doc-comment bNewSeedPending. Seed намеренно НЕ
		// перекатывается здесь: он должен смениться ровно один раз, в момент
		// фактического реролла, иначе несколько отложенных нажатий сожгли бы
		// несколько сидов, а показали бы только последний.
		bNewSeedPending = true;
		bResetToInitialStatePending = false;
		UE_LOG(LogTemp, Warning, TEXT("NewSeed: фоновый шаг StepAsync() ещё считается - новый сид отложен до его завершения"));
		return;
	}

	Seed = FMath::Rand();
	// Тем же генератором, что и старт с хоткеем Y: N теперь означает "та же
	// фигура, другой сид", а не "случайный шар вместо того, что настроено".
	// Прежнее поведение доступно выбором EStateGeneratorType::RandomBall.
	GenerateState();
}

bool AAutomataOrchestrator::TryAutoReseedOnExtinction(int32 GenerationsAdvanced)
{
	if (!bAutoReseedOnExtinction || !Grid.IsValid() || Grid->Num() != 0)
	{
		return false;
	}

	++AutoReseedCount;

	// Сколько поколений прожил сид, пишем ДО реролла - это единственное, что
	// про него интересно, а GenerationCount вот-вот обнулится вместе с сеткой.
	// Прибавка здесь потому, что вызывающая сторона до своего счётчика ещё не
	// дошла: проверка стоит раньше, чтобы не платить за рендер пустоты.
	UE_LOG(LogTemp, Log, TEXT("Автоперекат сида: сид %d вымер на поколении %lld, попытка №%d"),
		Seed, GenerationCount + GenerationsAdvanced, AutoReseedCount);

	NewSeed();
	return true;
}

void AAutomataOrchestrator::SetAutoReseedOnExtinction(bool bEnable)
{
	bAutoReseedOnExtinction = bEnable;
	AutoReseedCount = 0;

	if (!bEnable)
	{
		UE_LOG(LogTemp, Log, TEXT("SetAutoReseedOnExtinction: автоперекат сида выключен"));
		ShowStatusMessage(StatusKey_Generation, TEXT("Автоперекат сида: выключен"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("SetAutoReseedOnExtinction: автоперекат сида включён - вымершая сетка будет пересеиваться сама"));
	ShowStatusMessage(StatusKey_Generation, TEXT("Автоперекат сида: включён - вымершая сетка пересеивается сама"));

	// Режим включают в том числе ПОСЛЕ того, как всё погасло (смотрел, как
	// умирает, и решил перебирать дальше). Ждать в этом случае нечего: шагов
	// больше не будет - мёртвая сетка мертва и на следующем поколении, - так что
	// первый сид катим прямо здесь. NewSeed() сам отложится, если прямо сейчас
	// считается фоновый шаг (bNewSeedPending).
	if (Grid.IsValid() && Grid->Num() == 0)
	{
		++AutoReseedCount;
		NewSeed();
	}

	// И сразу запускаем прогон, если он не идёт: на паузе перебирать нечего -
	// вымирает только то, что считается. "Включил перебор" и "начал
	// перебирать" - одно действие, а не два, иначе Shift+N в самом типичном
	// случае (пришёл к мёртвой сетке, включил режим) внешне не делал бы ничего.
	//
	// Автошаг по Shift+F не трогаем: он тоже считает поколения, то есть перебор
	// уже идёт, а Start() при нём всё равно откажется (см. его реализацию).
	if (!bSimulationRunning && !IsFastStepActive())
	{
		Start();
	}
}

void AAutomataOrchestrator::AdjustSpeed(float Delta)
{
	// Верхняя граница здесь выше, чем UIMax в UPROPERTY-метаданных Speed
	// (10.0) - тот UIMax только ограничивает слайдер в Details panel, не сам
	// ClampMax, так что хоткеям +/- можно позволить разогнать Speed дальше.
	Speed = FMath::Clamp(Speed + Delta, 0.1f, 100.0f);
	UE_LOG(LogTemp, Log, TEXT("AdjustSpeed: Speed = %.2f"), Speed);
}

void AAutomataOrchestrator::SetSpeed(float NewSpeed)
{
	// Тот же кламп, что в AdjustSpeed() (см. комментарий там про то, почему
	// верхняя граница шире UIMax свойства).
	Speed = FMath::Clamp(NewSpeed, 0.1f, 100.0f);
	UE_LOG(LogTemp, Log, TEXT("SetSpeed: Speed = %.2f"), Speed);
}

void AAutomataOrchestrator::AdjustStepsPerRender(int32 Delta)
{
	SetStepsPerRender(StepsPerRender + Delta);
}

void AAutomataOrchestrator::SetStepsPerRender(int32 NewStepsPerRender)
{
	StepsPerRender = FMath::Clamp(NewStepsPerRender, 1, MaxStepsPerRender);
	UE_LOG(LogTemp, Log, TEXT("SetStepsPerRender: StepsPerRender = %d"), StepsPerRender);
}

void AAutomataOrchestrator::ScaleStepsPerRender(bool bDouble)
{
	// Не умножение на два, а переход к следующей/предыдущей СТЕПЕНИ ДВОЙКИ:
	// если текущее значение степенью двойки не является (например 254,
	// набранное с клавиши), удвоение оставило бы его таким же неровным - 508.
	// Так же одно нажатие всегда приводит на 2^k, откуда дальше можно ходить
	// по степеням точно.
	const int32 Current = FMath::Clamp(StepsPerRender, 1, MaxStepsPerRender);

	int32 Next;
	if (bDouble)
	{
		Next = 1;
		while (Next <= Current && Next < MaxStepsPerRender)
		{
			Next <<= 1;
		}
	}
	else
	{
		Next = MaxStepsPerRender;
		while (Next >= Current && Next > 1)
		{
			Next >>= 1;
		}
	}

	SetStepsPerRender(Next);
}

void AAutomataOrchestrator::Next()
{
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: фоновый шаг StepAsync() ещё считается - подождите его завершения"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: сетка не инициализирована - сначала постройте состояние (хоткей Y / GenerateState)"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: активный CellsMesh-компонент отсутствует"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("Next: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	// Строим правило заново на каждый вызов, чтобы правки BirthCounts/
	// SurvivalCounts/Neighborhood в Details panel подхватывались немедленно
	// (аналогично тому, как GenerateRandom() каждый раз пересоздаёт Grid,
	// а не кэширует его)
	FCellularAutomatonRule AutomatonRule = BuildRule();
	TUniquePtr<FCellularAutomatonComputeStrategy> ComputeStrategy = CreateComputeStrategy();

	// Ручной шаг считает StepsPerRender поколений за одно нажатие (то же
	// значение, что крутится хоткеями T/G) и рендерит только итоговое -
	// промежуточные поколения на экран не попадают, ровно как поколения,
	// пропускаемые StepsPerRender'ом в непрерывном Play. При
	// StepsPerRender == 1 поведение прежнее: один шаг - один рендер.
	const int32 NumSteps = FMath::Max(1, StepsPerRender);

	// Счёт уходит в фоновый пул потоков, как и в StepAsync() - раньше Next()
	// считал синхронно на game thread, и с NumSteps > 1 нажатие F замораживало
	// экран на всё время счёта (в Play такого нет именно потому, что там счёт
	// фоновый). Промежуточные буферы поколений создаются уже в фоне, поэтому
	// геометрию решётки и ChunkSize (UPROPERTY, могут править в Details panel)
	// снимаем здесь - фоновый поток не должен их читать.
	const FLatticeTransform LatticeSnapshot = BuildLatticeTransform();
	const int32 ChunkSizeSnapshot = ChunkSize;

	bStepInProgress = true;

	FCellGrid* CurrentGridPtr = Grid.Get();
	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	// Сырой указатель на *Grid без защиты времени жизни - как и в StepAsync(),
	// EndPlay() дожидается PendingStepFuture перед разрушением актора, а все
	// остальные пути замены Grid отказываются работать при bStepInProgress.
	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[AutomatonRule = MoveTemp(AutomatonRule), ComputeStrategy = MoveTemp(ComputeStrategy),
		 CurrentGridPtr, WeakThis, NumSteps, LatticeSnapshot, ChunkSizeSnapshot]() mutable
		{
			const double StepStartSeconds = FPlatformTime::Seconds();

			// StepBatch() вместо Step(): стратегия, умеющая считать несколько
			// поколений за один свой внутренний круг (GPU - см. её
			// doc-comment), берёт столько, сколько может, и говорит, сколько
			// реально продвинула; CPU-стратегия всегда возвращает 1, и цикл
			// вырождается в прежний "по одному поколению за итерацию".
			TUniquePtr<FCellGrid> ResultGrid;
			const FCellGrid* SourceGrid = CurrentGridPtr;
			int32 StepsDone = 0;
			while (StepsDone < NumSteps)
			{
				TUniquePtr<FCellGrid> NextGrid = MakeUnique<FDenseCellGrid>(LatticeSnapshot, ChunkSizeSnapshot, AutomatonRule.HasDecayStates());
				const int32 StepsAdvanced = ComputeStrategy->StepBatch(*SourceGrid, *NextGrid, AutomatonRule, NumSteps - StepsDone);

				// Оба прохода умеют продвинуть состояние только с одного
				// поколения на СОСЕДНЕЕ, а внутри пачки промежуточных не
				// существует - стратегия, продвинувшая больше одного, обязана
				// была заполнить и возрасты, и угасание сама (см. её
				// doc-comment). Позвать их поверх этого значило бы затереть
				// верные значения неверными.
				if (StepsAdvanced <= 1)
				{
					CellAging::ComputeAges(SourceGrid, *NextGrid);
					CellDecay::AdvanceDecayStates(SourceGrid, *NextGrid, AutomatonRule.GetStates());
				}

				ResultGrid = MoveTemp(NextGrid);
				SourceGrid = ResultGrid.Get();
				StepsDone += FMath::Max(1, StepsAdvanced);
			}

			const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;

			// Снимаем ещё здесь, в фоновом потоке, пока ComputeStrategy жива -
			// она уничтожится вместе с этой лямбдой, дальше её не будет
			// (см. FHudStats::EstimatedGpuComputeUploadMB). Отражает только
			// ПОСЛЕДНИЙ из NumSteps шагов - для HUD-индикатора этого достаточно.
			const int64 ComputeUploadBytes = ComputeStrategy->GetLastComputeUploadBytes();

			// Grid/рендер трогаем только на game thread (см. StepAsync()).
			AsyncTask(ENamedThreads::GameThread, [WeakThis, ResultGrid = MoveTemp(ResultGrid), StepSeconds, NumSteps, ComputeUploadBytes]() mutable
			{
				AAutomataOrchestrator* StrongThis = WeakThis.Get();
				if (!StrongThis)
				{
					return;
				}

				StrongThis->Grid = MoveTemp(ResultGrid);
				StrongThis->SelectedCells.Reset();
				StrongThis->bStepInProgress = false;

				// Тот же отложенный сброс, что и в ApplyStepResult() - см.
				// doc-comment bResetToInitialStatePending.
				if (StrongThis->bResetToInitialStatePending)
				{
					StrongThis->bResetToInitialStatePending = false;
					StrongThis->ResetToInitialState();
					return;
				}

				// Отложенный реролл (N) - см. doc-comment bNewSeedPending.
				if (StrongThis->bNewSeedPending)
				{
					StrongThis->bNewSeedPending = false;
					StrongThis->NewSeed();
					return;
				}

				// Отложенный шаг назад (Ctrl+Z) - до увеличения GenerationCount
				// ниже, по той же причине, что и в ApplyStepResult().
				if (StrongThis->bStepBackwardPending)
				{
					StrongThis->bStepBackwardPending = false;
					StrongThis->StepBackward();
					return;
				}

				// Вымирание ловится и на ручном шаге - см.
				// bAutoReseedOnExtinction и ту же проверку в ApplyStepResult().
				if (StrongThis->TryAutoReseedOnExtinction(NumSteps))
				{
					return;
				}

				// NumSteps реально посчитанных поколений за одно нажатие F -
				// см. GenerationCount/FHudStats.
				StrongThis->GenerationCount += NumSteps;

				// Шаг гасит повтор - та же причина, что в ApplyStepResult().
				StrongThis->EditRedoStack.Reset();
				StrongThis->LastGpuComputeUploadBytes = ComputeUploadBytes;

				// Точка графика. Ручной шаг всегда рисует (ниже), так что
				// перенесённое сюда значение "видимо" тут же исправится на
				// фактическое - но появиться замер обязан здесь, рядом со
				// счётчиком, а не в рендере: так одно и то же место отвечает
				// за "поколение состоялось" в обеих ветках, ручной и Play.
				StrongThis->AppendGenerationSample();

				// Ghost Shape пересчитывается по своему отдельному интервалу
				// поколений - см. ApplyStepResult() и план "Ghost Shape".
				if (StrongThis->bEnableGhostShape)
				{
					StrongThis->GhostShapeGenerationsSinceRefresh += NumSteps;
					if (StrongThis->GhostShapeGenerationsSinceRefresh >= FMath::Max(1, StrongThis->GhostShapeRefreshInterval))
					{
						StrongThis->GhostShapeGenerationsSinceRefresh = 0;
						StrongThis->RefreshGhostShape();
					}
				}

				// Всегда немедленно и целиком, в отличие от ApplyStepResult() -
				// ручной шаг игнорирует и bEnableChunkedRender, и счётчик
				// StepsSinceLastRender (пропуск рендера здесь уже "прожит"
				// самим циклом NumSteps выше).
				StrongThis->RenderGridImmediate();

				UE_LOG(LogTemp, Log, TEXT("Next: живых клеток %d после %d шаг(ов) (счёт: %.2f мс [фоновый поток])"),
					StrongThis->Grid->Num(), NumSteps, StepSeconds * 1000.0);
			});
		});
}

void AAutomataOrchestrator::StepAsync()
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: сетка не инициализирована - сначала постройте состояние (хоткей Y / GenerateState)"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: активный CellsMesh-компонент отсутствует"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("StepAsync: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	// Правило, стратегия расчёта и буфер следующего поколения строим здесь,
	// на game thread - все три читают UPROPERTY (BirthCounts/SurvivalCounts/
	// Neighborhood/ComputeMethod/GpuVolumeCellLimit/CellSize/ChunkSize),
	// которые могут одновременно редактироваться в Details panel. После этой
	// точки фоновый поток их больше не касается - только *Grid (на чтение) и
	// NextGridBuffer (на запись, свежесозданный, ни с кем не общий).
	FCellularAutomatonRule AutomatonRule = BuildRule();
	TUniquePtr<FCellularAutomatonComputeStrategy> ComputeStrategy = CreateComputeStrategy();
	TUniquePtr<FCellGrid> NextGridBuffer = CreateGrid();

	// Сколько поколений посчитать за ОДИН фоновый заход. Больше одного - только
	// если стратегия действительно умеет пачки для этого правила (см.
	// FCellularAutomatonComputeStrategy::SupportsStepBatching()): тогда
	// StepsPerRender поколений считаются за один круг через GPU и рендерится
	// итог, вместо StepsPerRender отдельных заходов, из которых рисуется
	// последний. Если не умеет (CPU-стратегия, либо GPU в режиме Generations) -
	// остаётся ровно прежний ритм "одно поколение за заход", вместе со всей
	// логикой пропуска рендеров по StepsSinceLastRender: собирать поколения в
	// пачку там незачем, работа та же, но одним длинным блоком.
	const int32 BatchGenerations = ComputeStrategy->SupportsStepBatching(AutomatonRule)
		? FMath::Max(1, StepsPerRender)
		: 1;

	// Промежуточные буферы поколений (нужны только при BatchGenerations > 1)
	// создаются уже в фоне, поэтому геометрия решётки и ChunkSize - живые
	// UPROPERTY, которые фоновому потоку трогать нельзя - снимаем здесь. Тот
	// же приём, что в Next().
	const FLatticeTransform LatticeSnapshot = BuildLatticeTransform();
	const int32 ChunkSizeSnapshot = ChunkSize;

	bStepInProgress = true;

	FCellGrid* CurrentGridPtr = Grid.Get();
	TWeakObjectPtr<AAutomataOrchestrator> WeakThis(this);

	// CurrentGridPtr - сырой указатель на *Grid, без защиты времени жизни -
	// PendingStepFuture даёт EndPlay() дождаться завершения этого фонового
	// шага перед тем, как актор (а с ним и Grid) начнёт разрушаться.
	PendingStepFuture = Async(EAsyncExecution::ThreadPool,
		[AutomatonRule = MoveTemp(AutomatonRule), ComputeStrategy = MoveTemp(ComputeStrategy),
		 NextGridBuffer = MoveTemp(NextGridBuffer), CurrentGridPtr, WeakThis,
		 BatchGenerations, LatticeSnapshot, ChunkSizeSnapshot]() mutable
		{
			const double StepStartSeconds = FPlatformTime::Seconds();

			// При BatchGenerations == 1 (CPU-стратегия / Generations) цикл
			// выполняется ровно один раз и ничего лишнего не аллоцирует -
			// путь остаётся прежним. Тот же порядок вызовов и то же условие
			// пропуска ComputeAges(), что в Next(): продвинувшая больше одного
			// поколения стратегия обязана была заполнить возрасты сама.
			TUniquePtr<FCellGrid> PreviousGrid;
			const FCellGrid* SourceGrid = CurrentGridPtr;
			int32 GenerationsAdvanced = 0;
			while (true)
			{
				const int32 StepsAdvanced = ComputeStrategy->StepBatch(*SourceGrid, *NextGridBuffer, AutomatonRule, BatchGenerations - GenerationsAdvanced);

				if (StepsAdvanced <= 1)
				{
					CellAging::ComputeAges(SourceGrid, *NextGridBuffer);
					CellDecay::AdvanceDecayStates(SourceGrid, *NextGridBuffer, AutomatonRule.GetStates());
				}

				GenerationsAdvanced += FMath::Max(1, StepsAdvanced);

				// Выходим и когда набрали всю пачку, и когда стратегия
				// фактически НЕ пачкует. Второе - не теория: стратегия отвечает
				// на SupportsStepBatching() один раз за заход, а влезает ли
				// пачка, решается уже внутри StepBatch() по текущему объёму
				// AABB, который растёт вместе с сеткой. Дорастив объём до
				// потолка, пачка урезается до 1 - и без этого выхода цикл
				// намолотил бы BatchGenerations одиночных шагов внутри ОДНОГО
				// фонового захода: та же работа, но одним блоком на несколько
				// секунд, с висящим всё это время bStepInProgress (он блокирует
				// R и генерацию) и с прерванным чанковым разливом. Наблюдалось
				// живьём: на 11 млн клеток такой заход занял 7.7 с. Возврат к
				// прежнему ритму "одно поколение за заход" здесь строго лучше -
				// следующее посчитается следующим Tick()'ом.
				if (StepsAdvanced <= 1 || GenerationsAdvanced >= BatchGenerations)
				{
					break;
				}

				// Только что посчитанное поколение становится источником для
				// следующего - и должно оставаться живым, пока в него читают,
				// поэтому владение переезжает в PreviousGrid, а не теряется.
				PreviousGrid = MoveTemp(NextGridBuffer);
				SourceGrid = PreviousGrid.Get();
				NextGridBuffer = MakeUnique<FDenseCellGrid>(LatticeSnapshot, ChunkSizeSnapshot, AutomatonRule.HasDecayStates());
			}

			const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;

			// Снимаем ещё здесь, пока ComputeStrategy жива (уничтожится вместе
			// с этой лямбдой) - см. FHudStats::EstimatedGpuComputeUploadMB.
			const int64 ComputeUploadBytes = ComputeStrategy->GetLastComputeUploadBytes();

			// Grid/рендер трогаем только на game thread - AsyncTask сюда и
			// маршрутизирует. WeakThis - на случай, если актор уничтожили
			// (например, level unload) пока фоновый Step() ещё считался.
			AsyncTask(ENamedThreads::GameThread, [WeakThis, NextGridBuffer = MoveTemp(NextGridBuffer), StepSeconds, ComputeUploadBytes, GenerationsAdvanced]() mutable
			{
				if (AAutomataOrchestrator* StrongThis = WeakThis.Get())
				{
					StrongThis->ApplyStepResult(MoveTemp(NextGridBuffer), StepSeconds, ComputeUploadBytes, GenerationsAdvanced);
				}
			});
		});
}

void AAutomataOrchestrator::ApplyStepResult(TUniquePtr<FCellGrid> NewGrid, double StepSeconds, int64 ComputeUploadBytes, int32 GenerationsAdvanced)
{
	// Один фоновый заход мог посчитать сразу несколько поколений (см.
	// BatchGenerations в StepAsync()) - все счётчики ниже считают ПОКОЛЕНИЯ,
	// а не заходы, поэтому идут шагом GenerationsAdvanced. При обычном
	// одиночном шаге это 1, и поведение прежнее.
	const int32 Generations = FMath::Max(1, GenerationsAdvanced);

	// Темп следующих заходов - по ФАКТИЧЕСКОМУ размеру этого, а не по тому,
	// что планировалось до дispatch'а: пачка могла быть урезана внутри
	// стратегии (объём AABB упёрся в её потолок), и тогда ждать
	// StepsPerRender/Speed ради одного посчитанного поколения значило бы
	// замедлить симуляцию ровно в StepsPerRender раз. Так интервал сам
	// сходится к реальности за один заход - в обе стороны.
	LastDispatchGenerations = Generations;

	Grid = MoveTemp(NewGrid);
	LastGpuComputeUploadBytes = ComputeUploadBytes;
	// Новое поколение делает старое выделение бессмысленным - сбрасываем
	// сразу, независимо от того, дойдёт ли дело до фактического рендера ниже
	// (см. doc-comment SelectedCells в заголовке).
	SelectedCells.Reset();

	// Сужено до конца фонового чтения Grid - дальше (рендер, возможный
	// чанковый "разлив") фонового потока уже не касается, так что следующий
	// StepAsync() может стартовать независимо от того, что происходит с
	// рендером ниже.
	bStepInProgress = false;

	// R, нажатый пока этот шаг ещё считался, был отложен (см. doc-comment
	// bResetToInitialStatePending) - гонка на Grid позади, выполняем его
	// сейчас вместо обычного применения только что посчитанного поколения
	// (которое всё равно тут же было бы перезаписано сбросом).
	if (bResetToInitialStatePending)
	{
		bResetToInitialStatePending = false;
		ResetToInitialState();
		return;
	}

	// То же для N, нажатой во время этого шага - реролл вместо применения
	// только что посчитанного поколения (оно всё равно было бы перезаписано
	// новой случайной сеткой). См. doc-comment bNewSeedPending.
	if (bNewSeedPending)
	{
		bNewSeedPending = false;
		NewSeed();
		return;
	}

	// То же для Ctrl+Z (см. doc-comment bStepBackwardPending). Стоит ДО
	// увеличения GenerationCount ниже, и это принципиально: StepBackward()
	// отсчитывает от него, а поколение, только что посчитанное этим самым
	// заходом, на экране ещё не было. Учтя его, откат вернул бы ровно то, что
	// сейчас в Grid, и нажатие не изменило бы ничего видимого.
	if (bStepBackwardPending)
	{
		bStepBackwardPending = false;
		StepBackward();
		return;
	}

	// Сетка вымерла, а режим брутфорса включён - катим следующий сид вместо
	// того, чтобы рисовать пустоту (см. bAutoReseedOnExtinction). Проверка
	// стоит ПЕРЕД счётчиками и рендером ниже, потому что NewSeed() всё равно
	// перестроит сетку с нуля и обнулит их (RebuildGridFromCells()).
	if (TryAutoReseedOnExtinction(Generations))
	{
		return;
	}

	// Реально посчитанные поколения - считаем для HUD независимо от того,
	// пропустит ли StepsSinceLastRender ниже фактический рендер (см.
	// GenerationCount/FHudStats).
	GenerationCount += Generations;

	// Шаг симуляции - тоже новое действие: правка, снятая с поколения, которое
	// осталось позади, накатилась бы не туда (см. doc-comment EditRedoStack).
	EditRedoStack.Reset();

	// Точка графика - здесь же, ДО обеих проверок пропуска рендера ниже, по
	// той же причине, по которой тут стоят серийная съёмка и Ghost Shape:
	// линия "всего клеток" описывает симуляцию, а не экран, и обязана
	// существовать для поколений, до AddInstances() не дошедших. Значение
	// "видимо" переносится с прошлого замера и исправляется на фактическое
	// в RenderCurrentGrid() ниже, если это поколение всё-таки рисуется.
	AppendGenerationSample();

	// Серия снимков идёт по своему счётчику ПОКОЛЕНИЙ - как и Ghost Shape
	// ниже, и по той же причине: шагом заходов было бы неравномерно (один
	// заход может посчитать сразу пачку), а шагом кадров экрана - зависело бы
	// от скорости отрисовки. Съёмка не смотрит на StepsSinceLastRender: она
	// растеризует сетку сама и не нуждается в том, чтобы поколение попало на
	// экран.
	//
	// Решение "рисовать ли это поколение" снимается ЗДЕСЬ, до съёмки, а не в
	// самой проверке ниже: последний кадр серии заканчивается вызовом
	// StopSeriesCapture() прямо из CaptureSeriesFrame(), и тот сбрасывает
	// bSeriesCaptureActive. Прочитанный после этого флаг сказал бы "серии нет",
	// и финальное поколение - единственное из всех - уехало бы в AddInstances,
	// хотя оно уже лежит в последнем PNG.
	const bool bSeriesSkipsRender = bSeriesCaptureActive && SliceCaptureParams.bSeriesFastMode;
	if (bSeriesCaptureActive)
	{
		SeriesGenerationsSinceFrame += Generations;
		if (SeriesGenerationsSinceFrame >= FMath::Max(1, SliceCaptureParams.SeriesGenerationsPerFrame))
		{
			SeriesGenerationsSinceFrame = 0;
			CaptureSeriesFrame();
		}
	}

	// Ghost Shape пересчитывается по своему отдельному интервалу поколений,
	// независимо от StepsPerRender - см. план "Ghost Shape".
	if (bEnableGhostShape)
	{
		GhostShapeGenerationsSinceRefresh += Generations;
		if (GhostShapeGenerationsSinceRefresh >= FMath::Max(1, GhostShapeRefreshInterval))
		{
			GhostShapeGenerationsSinceRefresh = 0;
			RefreshGhostShape();
		}
	}

	// Серия в быстром режиме не рисует промежуточные поколения вовсе: снимок
	// растеризуется прямо из сетки, и поколению незачем попадать на экран,
	// чтобы попасть в файл, а рендер клеток - самая дорогая часть кадра.
	// Экран так и остаётся на состоянии, с которого серию запустили, - в том
	// числе после её окончания (см. StopSeriesCapture() и bSeriesSkipsRender
	// выше: флаг снят до съёмки, поэтому последнее поколение серии тоже сюда
	// не проходит).
	if (bSeriesSkipsRender)
	{
		return;
	}

	// Шагом в Generations, а не на единицу: когда заход посчитал целую пачку
	// из StepsPerRender поколений, порог достигается тем же самым условием,
	// и рендерится каждый такой заход - отдельной ветки "пачка рендерит
	// всегда" не нужно.
	StepsSinceLastRender += Generations;
	if (StepsSinceLastRender < StepsPerRender)
	{
		UE_LOG(LogTemp, Log, TEXT("StepAsync: живых клеток %d после шага (шаг: %.2f мс [фоновый поток]) - рендер пропущен (%d/%d)"),
			Grid->Num(), StepSeconds * 1000.0, StepsSinceLastRender, StepsPerRender);
		return;
	}
	StepsSinceLastRender = 0;

	// Если предыдущий чанковый "разлив" ещё не дорисовался - по умолчанию не
	// ждём его окончания, а прерываем немедленно: RenderCurrentGrid() ниже
	// вызывает BeginRender(), который сам делает ClearInstances() и
	// перестраивает PendingTransforms с нуля по уже подставленному Grid, так
	// что недорисованные инстансы прошлого поколения просто никогда не
	// попадут на экран. Пока включён bWaitForChunkedRenderToFinish, эта ветка
	// физически не должна срабатывать - Tick() уже не запускает StepAsync(),
	// пока bChunkedRenderInProgress истинен (см. bBlockedByChunkedRender в
	// Tick()), так что сюда мы попадаем только с уже завершённым разливом.
	if (bChunkedRenderInProgress)
	{
		UE_LOG(LogTemp, Log, TEXT("StepAsync: живых клеток %d после шага (шаг: %.2f мс [фоновый поток]) - предыдущий разлив прерван, рендерим новое состояние"),
			Grid->Num(), StepSeconds * 1000.0);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("StepAsync: %d поколени(й) за заход, %.2f мс [фоновый поток]"), Generations, StepSeconds * 1000.0);
	}
	RenderCurrentGrid();
}

void AAutomataOrchestrator::StartFastStep()
{
	if (bSimulationRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFastStep: симуляция уже запущена через Play (пробел) - остановите её сначала"));
		return;
	}

	bFastStepActive = true;
	TimeSinceLastStep = 0.0f;
	StepsSinceLastRender = 0;
	SetActorTickEnabled(true);

	UE_LOG(LogTemp, Log, TEXT("StartFastStep: автошаг (Shift+F) включён"));
}

void AAutomataOrchestrator::StopFastStep()
{
	bFastStepActive = false;

	// bEnableViewSlice - ещё один потребитель тика помимо симуляции: срез
	// следит за камерой и на паузе (см. SetViewSliceEnabled()).
	if (!bSimulationRunning && !bChunkedRenderInProgress && !bEnableViewSlice)
	{
		SetActorTickEnabled(false);
	}

	UE_LOG(LogTemp, Log, TEXT("StopFastStep: автошаг выключен"));
}

void AAutomataOrchestrator::Start()
{
	if (IsFastStepActive())
	{
		UE_LOG(LogTemp, Warning, TEXT("Start: активен автошаг по F - остановите его (повторное F / отпустить Shift+F) перед запуском Play"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("Start game"));
	Resume();

	if (!Grid)
	{
		// Play по пустой сцене сначала строит состояние - тем же генератором,
		// что старт и N.
		GenerateState();
	}

	TimeSinceLastStep = 0.0f;
	StepsSinceLastRender = 0;
	bSimulationRunning = true;
	SetActorTickEnabled(true);
}

void AAutomataOrchestrator::Pause()
{
	if (!GamePC)
	{
		UE_LOG(LogTemp, Warning, TEXT("Pause: GamePC не назначен - PlayerController не найден"));
		return;
	}
	GamePC->SetCameraControlEnabled(false);
}
void AAutomataOrchestrator::Resume()
{
	if (!GamePC)
	{
		UE_LOG(LogTemp, Warning, TEXT("Resume: GamePC не назначен - PlayerController не найден"));
		return;
	}
	GamePC->SetCameraControlEnabled(true);
}

void AAutomataOrchestrator::Stop()
{
	bSimulationRunning = false;

	// Если чанковый рендер ещё не доехал по кадрам - досыпаем его одним
	// разом сейчас, а не оставляем недорисованным: SetActorTickEnabled(false)
	// ниже иначе останавливает Tick() (а с ним и AdvanceChunkedRender())
	// прямо здесь, замораживая "разлив" навсегда до следующего Start()/шага.
	FinishChunkedRenderImmediately();

	// Не безусловный false: срез вдоль взгляда следит за камерой и на паузе,
	// а без тика он застыл бы (см. SetViewSliceEnabled()).
	SetActorTickEnabled(bEnableViewSlice);
}

void AAutomataOrchestrator::Clear()
{
	
}