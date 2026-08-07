// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Generation/CellArrayModifier.h"
#include "Automata/Generation/StateGenerators.h"
#include "Automata/Generation/StateGeneratorPresets.h"
#include "Async/Async.h"


bool AAutomataOrchestrator::CanGenerateNewState(const TCHAR* LogPrefix) const
{
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: фоновый шаг StepAsync() ещё считается - подождите его завершения"), LogPrefix);
		return false;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: CellMesh не задан - назначьте StaticMesh в Details panel"), LogPrefix);
		return false;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: активный CellsMesh-компонент отсутствует"), LogPrefix);
		return false;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: CellMaterial не назначен - назначьте материал клеток в Details panel"), LogPrefix);
		return false;
	}

	return true;
}

void AAutomataOrchestrator::RebuildGridFromCells(TArray<FIntVector>&& Cells)
{
	// Новый прогон убирает запечённый меш-снимок, если он был (см.
	// BakeCellsToMesh()) - иначе новые клетки рисовались бы сквозь него.
	ClearBakedMesh();
	ClearGhostShape();

	// Всегда строим сетку с нуля - так подхватывается актуальный CellSize,
	// если его поменяли в Details panel.
	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	// Новый прогон - новый отсчёт поколений для HUD (см. GenerationCount/
	// FHudStats/ResetGenerationCounter()).
	ResetGenerationCounter();
	// Новая сетка делает старое выделение бессмысленным (координаты уже не
	// про эту сетку) - см. doc-comment SelectedCells в заголовке.
	SelectedCells.Reset();

	// Заливка строго последовательная: FCellGrid::SetAlive() кеширует последний
	// чанк и не потокобезопасен. Возраст намеренно не трогаем - ровно как
	// делал GenerateRandom() до появления генераторов (свежие чанки и так
	// зануляют Ages), в отличие от StartFromSelection(), который зовёт
	// SetAge() явно.
	for (const FIntVector& Cell : Cells)
	{
		Grid->SetAlive(Cell, true);
	}

	// Освобождаем массив генератора ДО того, как соберём InitialStateCells:
	// иначе на миллионах клеток пик держал бы оба массива разом.
	Cells.Empty();

	// Свежесгенерированное состояние - тоже валидная "точка возврата" R и
	// то, что уйдёт в файл при Save (см. doc-comment InitialStateCells) -
	// ровно как после StartFromSelection()/LoadStateFromFile(). Берём
	// фактически осевшие в сетке клетки, а не то, что отдал генератор:
	// у случайного шара броски дают коллизии в одну и ту же клетку.
	Grid->GetAliveCells(InitialStateCells);

	RenderGridImmediate();
}

void AAutomataOrchestrator::GenerateState()
{
	if (!CanGenerateNewState(TEXT("GenerateState")))
	{
		return;
	}

	const FString GeneratorName = StateGenerators::GetDisplayName(GenerationParams.Type);

	// Оценка ДО единого касания сетки: отказ обязан оставить текущее состояние
	// целым, а не стереть его и остановиться на полпути (та же идиома, что у
	// бюджета бейка - см. BakeCellsToMesh()).
	const int64 Estimate = StateGenerators::EstimateCellCount(GenerationParams);
	if (Estimate > MaxGeneratedCells)
	{
		const FString Message = FString::Printf(
			TEXT("Генератор '%s': ожидается %lld клеток при пределе %lld - уменьшите область или поднимите MaxGeneratedCells"),
			*GeneratorName, Estimate, MaxGeneratedCells);

		UE_LOG(LogTemp, Warning, TEXT("GenerateState: %s"), *Message);
		ShowStatusMessage(StatusKey_Generation, Message);
		return;
	}

	TArray<FIntVector> Cells;
	StateGenerators::FGenerateStats Stats;
	FString Error;

	if (!StateGenerators::Generate(GenerationParams, Seed, MaxGeneratedCells, Cells, Stats, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateState: %s"), *Error);
		ShowStatusMessage(StatusKey_Generation, FString::Printf(TEXT("Генерация не удалась: %s"), *Error));
		return;
	}

	// Гистограмму считаем ДО заливки, пока набор клеток ещё на руках.
	FString HistogramText;
	if (GenerationParams.bAnalyzeNeighborCounts)
	{
		StateGenerators::FNeighborHistogram Histogram;
		StateGenerators::AnalyzeNeighborCounts(Cells, BuildNeighborOffsetsForAnalysis(), NeighborAnalysisSampleExtent, Histogram);
		HistogramText = StateGenerators::DescribeHistogram(Histogram);
	}

	RebuildGridFromCells(MoveTemp(Cells));

	UE_LOG(LogTemp, Log, TEXT("GenerateState: '%s' - клеток %d (перебрано %lld, генерация: %.2f мс)"),
		*GeneratorName, Grid->Num(), Stats.ScannedCells, Stats.Seconds * 1000.0);

	if (!HistogramText.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("GenerateState: соседи по %s - %s"),
			GetNeighborhoodDisplayName(Neighborhood),
			*HistogramText);
	}

	ShowStatusMessage(StatusKey_Generation,
		FString::Printf(TEXT("Генератор: %s - %d клеток"), *GeneratorName, Grid->Num()));

	// Последним - чтобы предупреждение о несогласованности перекрыло собой
	// бодрое "построено N клеток": оно и есть главное, что нужно прочитать.
	WarnIfLifePatternMismatch();
}

void AAutomataOrchestrator::WarnIfLifePatternMismatch()
{
	if (GenerationParams.Type != EStateGeneratorType::LifePattern)
	{
		return;
	}

	// Генератор остаётся чистой геометрией (правило он не читает - см.
	// StateGenerators), но ОРКЕСТРАТОР знает и то, и другое, и промолчать здесь
	// нельзя: паттерн двумерной жизни, построенный не с тем правилом или не с
	// той толщиной, выглядит совершенно нормально и просто НЕ ОЖИВАЕТ - ни
	// ошибки, ни подсказки, ни единой зацепки, что искать.
	TArray<FString> Problems;

	if (GenerationParams.Thickness != 2)
	{
		Problems.Add(FString::Printf(TEXT("Thickness = %d, нужно 2"), GenerationParams.Thickness));
	}

	// Фильтр чётности выбросит ровно половину клеток паттерна - от фигуры
	// останется решето. Ловушка тихая: фильтр включают под ГЦК/ОЦК-опыты и
	// забывают.
	if (GenerationParams.ParityFilter != ECellParityFilter::None)
	{
		Problems.Add(TEXT("включён фильтр чётности - он выбросит половину клеток паттерна"));
	}

	if (Neighborhood != ENeighborhood::PlanarMoore)
	{
		Problems.Add(FString::Printf(TEXT("соседство %s, нужно PlanarMoore (плоскость XY + ось Z)"),
			GetNeighborhoodDisplayName(Neighborhood)));
	}

	if (States != 2)
	{
		Problems.Add(FString::Printf(TEXT("States = %d, нужно 2 (угасание ломает перенос)"), States));
	}

	// Правило: ровно S{3,4} B{3} - см. вывод формулы в doc-comment ELifePattern.
	TArray<int32> Survival = SurvivalCounts;
	TArray<int32> Birth = BirthCounts;
	Survival.Sort();
	Birth.Sort();
	const bool bRuleMatches = Survival == TArray<int32>({ 3, 4 }) && Birth == TArray<int32>({ 3 });
	if (!bRuleMatches)
	{
		Problems.Add(FString::Printf(TEXT("правило %s, нужно 3,4/3/2/PM (пресет \"Плоская жизнь\")"), *GetActiveRuleString()));
	}

	if (Problems.Num() == 0)
	{
		return;
	}

	const FString Message = FString::Printf(TEXT("Паттерн 2D-жизни не оживёт: %s"), *FString::Join(Problems, TEXT("; ")));
	UE_LOG(LogTemp, Warning, TEXT("GenerateState: %s"), *Message);
	ShowStatusMessage(StatusKey_Generation, Message);
}

void AAutomataOrchestrator::ArrayCells()
{
	if (!CanGenerateNewState(TEXT("ArrayCells")))
	{
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("ArrayCells: сетка не инициализирована"));
		return;
	}

	// Выделение, если оно есть, иначе вся сетка - см. doc-comment в заголовке.
	// Из выделения берём только реально живые клетки: оно переживает шаги
	// симуляции, и клетка под ним могла давно умереть (та же фильтрация, что в
	// StartFromSelection()/ComputeSelectedCellsBounds()).
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
			? TEXT("Тираж: в выделении нет живых клеток")
			: TEXT("Тираж: сетка пуста - размножать нечего");
		UE_LOG(LogTemp, Warning, TEXT("ArrayCells: %s"), *Message);
		ShowStatusMessage(StatusKey_Generation, Message);
		return;
	}

	// Оценка ДО единого касания сетки - та же идиома, что у GenerateState():
	// отказ обязан оставить текущее состояние целым. Здесь она к тому же точная
	// сверху и дешёвая: произведение трёх Count'ов на размер источника.
	const int64 Estimate = CellArrayModifier::EstimateCellCount(Source.Num(), ArrayParams);
	if (Estimate > MaxGeneratedCells)
	{
		const FString Message = FString::Printf(
			TEXT("Тираж: ожидается %lld клеток при пределе %lld - уменьшите Count или поднимите MaxGeneratedCells"),
			Estimate, MaxGeneratedCells);
		UE_LOG(LogTemp, Warning, TEXT("ArrayCells: %s"), *Message);
		ShowStatusMessage(StatusKey_Generation, Message);
		return;
	}

	const FIntVector SourceSize = CellArrayModifier::ComputeSize(Source);
	const FIntVector Step = CellArrayModifier::ComputeStep(SourceSize, ArrayParams);

	// Нулевой шаг по оси, которую при этом размножают, - не ошибка (все копии
	// лягут одна в одну и схлопнутся при заливке), но результат совпадёт с
	// источником, а выглядеть это будет как "хоткей ничего не сделал".
	if ((ArrayParams.Count.X > 1 && Step.X == 0)
		|| (ArrayParams.Count.Y > 1 && Step.Y == 0)
		|| (ArrayParams.Count.Z > 1 && Step.Z == 0))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ArrayCells: шаг %s - по оси с нулевым шагом копии лягут одна в одну; проверьте RelativeOffset/ConstantOffset"),
			*Step.ToString());
	}

	// Нечётный шаг уводит ГЦК/ОЦК-набор с его подрешётки: половина копий сядет
	// на соседнюю, и правило, замкнутое на подрешётке, поведёт себя на них
	// иначе. Молчать об этом нельзя - картинка выглядит правдоподобно (см.
	// ECellParityFilter и перенос при сохранении в ComputeCenteringOffset()).
	if (GenerationParams.ParityFilter != ECellParityFilter::None
		&& ((Step.X & 1) != 0 || (Step.Y & 1) != 0 || (Step.Z & 1) != 0))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ArrayCells: шаг %s нечётный при фильтре чётности %d - копии уедут на соседнюю подрешётку"),
			*Step.ToString(), static_cast<int32>(GenerationParams.ParityFilter));
	}

	const double StartSeconds = FPlatformTime::Seconds();
	TArray<FIntVector> Cells;
	CellArrayModifier::Tile(Source, ArrayParams, Cells);
	const double TileSeconds = FPlatformTime::Seconds() - StartSeconds;

	// Источник больше не нужен - освобождаем ДО заливки, иначе на миллионах
	// клеток пик держал бы и его, и тираж (та же причина, что в
	// RebuildGridFromCells()).
	Source.Empty();

	const int32 CopyCount = FMath::Max(ArrayParams.Count.X, 1)
		* FMath::Max(ArrayParams.Count.Y, 1)
		* FMath::Max(ArrayParams.Count.Z, 1);

	RebuildGridFromCells(MoveTemp(Cells));

	UE_LOG(LogTemp, Log,
		TEXT("ArrayCells: %d копий (%s), источник - %s, габарит %s, шаг %s; клеток %d (тираж: %.2f мс)"),
		CopyCount, *ArrayParams.Count.ToString(),
		bFromSelection ? TEXT("выделение") : TEXT("вся сетка"),
		*SourceSize.ToString(), *Step.ToString(), Grid->Num(), TileSeconds * 1000.0);

	ShowStatusMessage(StatusKey_Generation,
		FString::Printf(TEXT("Тираж: %d копий - %d клеток"), CopyCount, Grid->Num()));
}

void AAutomataOrchestrator::CycleStateGeneratorType()
{
	// Последний элемент перечисления - при добавлении нового генератора править
	// здесь, иначе Shift+Y просто не будет до него доходить, молча.
	const int32 TypeCount = static_cast<int32>(EStateGeneratorType::LifePattern) + 1;
	const int32 NextType = (static_cast<int32>(GenerationParams.Type) + 1) % TypeCount;
	GenerationParams.Type = static_cast<EStateGeneratorType>(NextType);

	const FString GeneratorName = StateGenerators::GetDisplayName(GenerationParams.Type);

	// Только переключаем тип, не генерируем: параметры нового типа почти всегда
	// хочется посмотреть и поправить до построения.
	UE_LOG(LogTemp, Log, TEXT("CycleStateGeneratorType: %s (оценка: %lld клеток)"),
		*GeneratorName, StateGenerators::EstimateCellCount(GenerationParams));

	ShowStatusMessage(StatusKey_Generation,
		FString::Printf(TEXT("Генератор: %s (~%lld клеток) - Y чтобы построить"),
			*GeneratorName, StateGenerators::EstimateCellCount(GenerationParams)));
}

TArray<FStateGeneratorPreset> AAutomataOrchestrator::GetStateGeneratorPresets() const
{
	return StateGeneratorPresets::GetAll();
}

void AAutomataOrchestrator::ApplyStateGeneratorPreset(int32 PresetIndex, bool bGenerateImmediately)
{
	const TArray<FStateGeneratorPreset>& Presets = StateGeneratorPresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyStateGeneratorPreset: индекс %d вне диапазона (пресетов: %d)"),
			PresetIndex, Presets.Num());
		return;
	}

	const FStateGeneratorPreset& Preset = Presets[PresetIndex];
	GenerationParams = Preset.Params;

	UE_LOG(LogTemp, Log, TEXT("ApplyStateGeneratorPreset: '%s' (%s) - оценка %lld клеток"),
		*Preset.Name, *Preset.FamilyName, StateGenerators::EstimateCellCount(GenerationParams));

	if (bGenerateImmediately)
	{
		GenerateState();
	}
}

void AAutomataOrchestrator::SetStateGeneratorParams(const FStateGeneratorParams& NewParams)
{
	GenerationParams = NewParams;

	// Клампы повторяют метаданные UPROPERTY: панель их соблюдает, а Blueprint
	// пишет в структуру напрямую и может занести что угодно.
	GenerationParams.Extent.X = FMath::Max(GenerationParams.Extent.X, 1);
	GenerationParams.Extent.Y = FMath::Max(GenerationParams.Extent.Y, 1);
	GenerationParams.Extent.Z = FMath::Max(GenerationParams.Extent.Z, 1);
	GenerationParams.Period.X = FMath::Max(GenerationParams.Period.X, 1);
	GenerationParams.Period.Y = FMath::Max(GenerationParams.Period.Y, 1);
	GenerationParams.Period.Z = FMath::Max(GenerationParams.Period.Z, 1);
	GenerationParams.CoreExtent.X = FMath::Max(GenerationParams.CoreExtent.X, 1);
	GenerationParams.CoreExtent.Y = FMath::Max(GenerationParams.CoreExtent.Y, 1);
	GenerationParams.CoreExtent.Z = FMath::Max(GenerationParams.CoreExtent.Z, 1);
	GenerationParams.BlockSize = FMath::Max(GenerationParams.BlockSize, 1);
	GenerationParams.Thickness = FMath::Max(GenerationParams.Thickness, 1);
	GenerationParams.Radius = FMath::Max(GenerationParams.Radius, 1);
	GenerationParams.Amount = FMath::Max(GenerationParams.Amount, 1);
	GenerationParams.ClusterCount = FMath::Max(GenerationParams.ClusterCount, 1);
	GenerationParams.ClusterRadius = FMath::Max(GenerationParams.ClusterRadius, 1);
	GenerationParams.Density = FMath::Clamp(GenerationParams.Density, 0.0f, 1.0f);
	GenerationParams.ClusterRadiusJitter = FMath::Clamp(GenerationParams.ClusterRadiusJitter, 0.0f, 0.9f);
	GenerationParams.NoiseScale = FMath::Max(GenerationParams.NoiseScale, 0.001f);
	GenerationParams.NoiseThreshold = FMath::Clamp(GenerationParams.NoiseThreshold, -1.0f, 1.0f);

	// FMath::PerlinNoise3D() возвращает ровно 0 в целочисленных точках, так что
	// "круглый" масштаб вырождает поле и даёт либо пустоту, либо сплошной куб -
	// молча отдать пустой результат тут хуже всего.
	if (GenerationParams.Type == EStateGeneratorType::NoisePerlin)
	{
		const float ScaleFraction = FMath::Frac(1.0f / FMath::Max(GenerationParams.NoiseScale, KINDA_SMALL_NUMBER));
		if (ScaleFraction < 0.01f || ScaleFraction > 0.99f)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("SetStateGeneratorParams: NoiseScale %.4f почти целократен - Perlin равен нулю в целых точках, результат может выйти пустым или сплошным"),
				GenerationParams.NoiseScale);
		}
	}
}

int64 AAutomataOrchestrator::EstimateStateGeneratorCells() const
{
	return StateGenerators::EstimateCellCount(GenerationParams);
}

FString AAutomataOrchestrator::GetStateGeneratorDisplayName() const
{
	return StateGenerators::GetDisplayName(GenerationParams.Type);
}
