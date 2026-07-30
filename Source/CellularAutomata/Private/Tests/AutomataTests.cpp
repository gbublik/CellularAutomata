#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Automata/Capture/CellRasterizer.h"
#include "Automata/Generation/StateGenerators.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Simulation/CellAging.h"
#include "Automata/Simulation/CellDecay.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/RuleStringParser.h"
#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"
#include "Automata/Simulation/ComputeStrategy/GpuComputeStrategy.h"
#include "Math/RandomStream.h"
#include "RHI.h"

/**
 * Автоматизационные тесты симуляции. Запуск headless (полный прогон занимает
 * столько же, сколько старт редактора - десятки секунд, отдельного быстрого
 * цикла в UE нет):
 *
 *   UnrealEditor-Cmd.exe CellularAutomata.uproject ^
 *     -ExecCmds="Automation RunTests CellularAutomata; Quit" ^
 *     -unattended -nopause -nosplash -log
 *
 * Тесты паритета (CpuGpuParity/GpuBatchParity) требуют НАСТОЯЩИЙ RHI - под
 * -nullrhi их запускать бессмысленно, поэтому они помечены NonNullRHI и
 * дополнительно сами проверяют GRHISupportsComputeShaders, чтобы на машине
 * без compute-поддержки честно сказать "пропущено", а не упасть.
 *
 * Что покрыто и почему именно это (см. CLAUDE.md за подробностями по каждому
 * механизму):
 *  - чанковая арифметика FDenseCellGrid на ОТРИЦАТЕЛЬНЫХ координатах - в
 *    проекте это явно отмеченная ловушка (FMath::DivideAndRoundDown не
 *    floor-деление), а генерация центрирована в нуле, т.е. отрицательные
 *    координаты - норма, а не крайний случай;
 *  - round-trip строкового правила: Parse -> Format -> Parse;
 *  - паритет CPU и GPU на одном и том же состоянии;
 *  - паритет "пачка из N поколений" против "N одиночных шагов" - прямая
 *    страховка для гало, возрастной плоскости и плоскости угасания
 *    (см. FGpuComputeStrategy::StepBatch()).
 */
namespace AutomataTestUtils
{
	/** Детерминированный аналог AAutomataOrchestrator::GenerateRandom():
	 *  reject-sampling точек в шаре, тем же FRandomStream, чтобы состояние
	 *  было воспроизводимым от прогона к прогону. */
	void SeedSphere(FCellGrid& Grid, int32 Seed, int32 Radius, int32 Amount)
	{
		FRandomStream RandomStream(Seed);
		const float RadiusInCells = static_cast<float>(Radius);

		for (int32 Index = 0; Index < Amount; ++Index)
		{
			FVector SamplePoint;
			do
			{
				SamplePoint = FVector(
					RandomStream.FRandRange(-RadiusInCells, RadiusInCells),
					RandomStream.FRandRange(-RadiusInCells, RadiusInCells),
					RandomStream.FRandRange(-RadiusInCells, RadiusInCells));
			}
			while (SamplePoint.SizeSquared() > FMath::Square(RadiusInCells));

			Grid.SetAlive(FIntVector(
				FMath::RoundToInt(SamplePoint.X),
				FMath::RoundToInt(SamplePoint.Y),
				FMath::RoundToInt(SamplePoint.Z)), true);
		}
	}

	/** Сравнивает две сетки покомпонентно: живые клетки, их возрасты и
	 *  угасающие состояния. Возвращает false и заполняет OutMismatch первым
	 *  найденным расхождением - для сообщения в тесте достаточно одного, но
	 *  конкретного. */
	bool GridsMatch(const FCellGrid& Left, const FCellGrid& Right, FString& OutMismatch)
	{
		if (Left.Num() != Right.Num())
		{
			OutMismatch = FString::Printf(TEXT("разное число живых клеток: %d против %d"), Left.Num(), Right.Num());
			return false;
		}

		TArray<FIntVector> LeftCells;
		Left.GetAliveCells(LeftCells);
		for (const FIntVector& Cell : LeftCells)
		{
			if (!Right.IsAlive(Cell))
			{
				OutMismatch = FString::Printf(TEXT("клетка (%d,%d,%d) жива только в одной из сеток"), Cell.X, Cell.Y, Cell.Z);
				return false;
			}
			if (Left.GetAge(Cell) != Right.GetAge(Cell))
			{
				OutMismatch = FString::Printf(TEXT("возраст (%d,%d,%d): %d против %d"),
					Cell.X, Cell.Y, Cell.Z, Left.GetAge(Cell), Right.GetAge(Cell));
				return false;
			}
		}

		TArray<FIntVector> LeftDecaying, RightDecaying;
		TArray<uint8> LeftStates, RightStates;
		Left.GetDecayingCells(LeftDecaying, LeftStates);
		Right.GetDecayingCells(RightDecaying, RightStates);
		if (LeftDecaying.Num() != RightDecaying.Num())
		{
			OutMismatch = FString::Printf(TEXT("разное число угасающих клеток: %d против %d"), LeftDecaying.Num(), RightDecaying.Num());
			return false;
		}
		for (int32 Index = 0; Index < LeftDecaying.Num(); ++Index)
		{
			const FIntVector& Cell = LeftDecaying[Index];
			if (Right.GetDecayState(Cell) != LeftStates[Index])
			{
				OutMismatch = FString::Printf(TEXT("стадия угасания (%d,%d,%d): %d против %d"),
					Cell.X, Cell.Y, Cell.Z, LeftStates[Index], Right.GetDecayState(Cell));
				return false;
			}
		}

		return true;
	}

	/** Одно поколение целиком, как его считает оркестратор: Step() плюс два
	 *  CPU-прохода после него (см. AAutomataOrchestrator::StepAsync()). */
	TUniquePtr<FCellGrid> AdvanceOneGeneration(const FCellGrid& Source, const FCellularAutomatonComputeStrategy& Strategy,
		const FCellularAutomatonRule& Rule, float CellSize, int32 ChunkSize)
	{
		TUniquePtr<FCellGrid> Next = MakeUnique<FDenseCellGrid>(CellSize, ChunkSize, Rule.HasDecayStates());
		Strategy.Step(Source, *Next, Rule);
		CellAging::ComputeAges(&Source, *Next);
		CellDecay::AdvanceDecayStates(&Source, *Next, Rule.GetStates());
		return Next;
	}

	/** Тесты, которым нужен настоящий GPU, должны уметь честно
	 *  самоустраниться: под -nullrhi или на платформе ниже SM5 падать нечему,
	 *  но и проверять нечего. Тот же порог, что в
	 *  FCellularAutomatonStepCS::ShouldCompilePermutation(). */
	bool IsGpuComputeAvailable()
	{
		return GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDenseCellGridNegativeCoordsTest,
	"CellularAutomata.Grid.NegativeCoords",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FDenseCellGridNegativeCoordsTest::RunTest(const FString& Parameters)
{
	constexpr int32 ChunkSize = 16;
	FDenseCellGrid Grid(100.0f, ChunkSize);

	// Координаты специально подобраны так, чтобы попасть по обе стороны от
	// нуля И на границы чанков: при обычном (усекающем к нулю) делении -1 и
	// 0 попали бы в один чанк, а -16 и -17 - не туда, куда нужно.
	const TArray<FIntVector> Cells = {
		FIntVector(0, 0, 0),
		FIntVector(-1, -1, -1),
		FIntVector(-16, -16, -16),
		FIntVector(-17, 0, 15),
		FIntVector(15, 15, 15),
		FIntVector(16, 16, 16),
		FIntVector(-100, 250, -333),
	};

	for (const FIntVector& Cell : Cells)
	{
		Grid.SetAlive(Cell, true);
	}

	TestEqual(TEXT("число живых клеток"), Grid.Num(), Cells.Num());

	for (const FIntVector& Cell : Cells)
	{
		TestTrue(FString::Printf(TEXT("клетка (%d,%d,%d) жива"), Cell.X, Cell.Y, Cell.Z), Grid.IsAlive(Cell));
	}

	// Соседи выставленных клеток обязаны остаться мёртвыми - иначе ошибка в
	// индексации внутри чанка выглядела бы как "всё работает".
	for (const FIntVector& Cell : Cells)
	{
		const FIntVector Neighbor = Cell + FIntVector(0, 0, 1);
		if (!Cells.Contains(Neighbor))
		{
			TestFalse(FString::Printf(TEXT("сосед (%d,%d,%d) мёртв"), Neighbor.X, Neighbor.Y, Neighbor.Z), Grid.IsAlive(Neighbor));
		}
	}

	TArray<FIntVector> AliveCells;
	Grid.GetAliveCells(AliveCells);
	TestEqual(TEXT("GetAliveCells() отдаёт столько же"), AliveCells.Num(), Cells.Num());
	for (const FIntVector& Cell : Cells)
	{
		TestTrue(FString::Printf(TEXT("GetAliveCells() содержит (%d,%d,%d)"), Cell.X, Cell.Y, Cell.Z), AliveCells.Contains(Cell));
	}

	// Возраст переживает запись/чтение, а вот опустошение чанка обязано
	// стереть историю: заново родившаяся в том же месте клетка стартует с 0
	// (см. doc-comment FDenseCellGrid::FChunk).
	const FIntVector AgeCell(-17, 0, 15);
	Grid.SetAge(AgeCell, 42);
	TestEqual(TEXT("возраст записался"), (int32)Grid.GetAge(AgeCell), 42);

	const FIntVector LonelyCell(-100, 250, -333);
	Grid.SetAge(LonelyCell, 7);
	Grid.SetAlive(LonelyCell, false);
	TestFalse(TEXT("клетка убита"), Grid.IsAlive(LonelyCell));
	Grid.SetAlive(LonelyCell, true);
	TestEqual(TEXT("возраст не унаследован от удалённого чанка"), (int32)Grid.GetAge(LonelyCell), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuleStringRoundTripTest,
	"CellularAutomata.Rules.RuleStringRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FRuleStringRoundTripTest::RunTest(const FString& Parameters)
{
	// Разбор: диапазоны раскрываются, поля идут в порядке Survival/Birth
	// (обратном порядку одноимённых свойств оркестратора - именно та ловушка,
	// из-за которой ApplyRuleString() присваивает по имени, а не позиционно).
	{
		RuleStringParser::FParsedRule Parsed;
		FString Error;
		TestTrue(TEXT("'0-6/1,3/2/VN' разбирается"), RuleStringParser::ParseRuleString(TEXT("0-6/1,3/2/VN"), Parsed, Error));
		TestEqual(TEXT("Survival раскрылся в диапазон"), Parsed.SurvivalCounts.Num(), 7);
		TestEqual(TEXT("Birth"), Parsed.BirthCounts.Num(), 2);
		TestTrue(TEXT("Birth содержит 1"), Parsed.BirthCounts.Contains(1));
		TestTrue(TEXT("Birth содержит 3"), Parsed.BirthCounts.Contains(3));
		TestEqual(TEXT("States"), Parsed.States, 2);
		TestTrue(TEXT("Neighborhood"), Parsed.Neighborhood == ENeighborhood::VonNeumann);
	}

	// Round-trip: Parse -> Format -> Parse даёт то же самое. Проверяем на
	// пресетных строках - именно они попадают в RuleString через
	// ApplyRulePreset().
	const TArray<FString> Rules = {
		TEXT("9-26/5-7,12-13,15/16/M"),
		TEXT("4/4/5/M"),
		TEXT("0-6/1,3/2/VN"),
		TEXT("8,11,13-26/13-26/5/M"),
		TEXT("6-8/6-8/3/M"),
	};

	for (const FString& Rule : Rules)
	{
		RuleStringParser::FParsedRule First;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("'%s' разбирается"), *Rule), RuleStringParser::ParseRuleString(Rule, First, Error)))
		{
			continue;
		}

		const FString Formatted = RuleStringParser::FormatRuleString(First.SurvivalCounts, First.BirthCounts, First.States, First.Neighborhood);

		RuleStringParser::FParsedRule Second;
		if (!TestTrue(FString::Printf(TEXT("'%s' (из '%s') разбирается обратно"), *Formatted, *Rule),
			RuleStringParser::ParseRuleString(Formatted, Second, Error)))
		{
			continue;
		}

		// TestEqual перегружен под скаляры/строки, но не под TArray - сравниваем
		// через TestTrue, оператор== у TArray поэлементный.
		TestTrue(FString::Printf(TEXT("'%s': Survival после round-trip"), *Rule), Second.SurvivalCounts == First.SurvivalCounts);
		TestTrue(FString::Printf(TEXT("'%s': Birth после round-trip"), *Rule), Second.BirthCounts == First.BirthCounts);
		TestEqual(FString::Printf(TEXT("'%s': States после round-trip"), *Rule), Second.States, First.States);
		TestTrue(FString::Printf(TEXT("'%s': Neighborhood после round-trip"), *Rule), Second.Neighborhood == First.Neighborhood);
	}

	// Сжатие обратно в диапазоны - не косметика: без него строка растёт с
	// каждым round-trip'ом.
	TestEqual(TEXT("подряд идущие сжимаются в диапазон"),
		RuleStringParser::FormatRuleString({ 1, 2, 3, 7 }, { 4 }, 2, ENeighborhood::Moore), FString(TEXT("1-3,7/4/2/M")));
	TestEqual(TEXT("повторы схлопываются"),
		RuleStringParser::FormatRuleString({ 5, 5, 5 }, { 4 }, 2, ENeighborhood::VonNeumann), FString(TEXT("5/4/2/VN")));

	// Битые строки обязаны отвергаться ЦЕЛИКОМ и с внятной ошибкой - принцип
	// "никогда не применять частично" (см. ApplyRuleString()).
	const TArray<FString> BadRules = {
		TEXT(""),
		TEXT("1/2/3"),
		TEXT("a/1/2/M"),
		TEXT("5-1/2/2/M"),
		TEXT("1/2/1/M"),
		TEXT("1/2/2/X"),
		TEXT("1//2/M"),
	};

	for (const FString& Bad : BadRules)
	{
		RuleStringParser::FParsedRule Parsed;
		FString Error;
		TestFalse(FString::Printf(TEXT("'%s' отвергается"), *Bad), RuleStringParser::ParseRuleString(Bad, Parsed, Error));
		TestFalse(FString::Printf(TEXT("'%s' даёт описание ошибки"), *Bad), Error.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCpuGpuParityTest,
	"CellularAutomata.Compute.CpuGpuParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCpuGpuParityTest::RunTest(const FString& Parameters)
{
	if (!AutomataTestUtils::IsGpuComputeAvailable())
	{
		AddInfo(TEXT("GPU-compute недоступен (нет RHI либо compute-шейдеров) - тест пропущен"));
		return true;
	}

	constexpr float CellSize = 100.0f;
	constexpr int32 ChunkSize = 16;
	constexpr int64 VolumeLimit = 512LL * 512LL * 512LL;

	// Два правила: бинарное и Generations - у них принципиально разные ветки
	// и в шейдере, и в CPU-стратегии.
	struct FRuleCase
	{
		const TCHAR* Name;
		TArray<int32> Birth;
		TArray<int32> Survival;
		ENeighborhood Neighborhood;
		int32 States;
	};

	const TArray<FRuleCase> Cases = {
		{ TEXT("бинарное 4/4/2/M"), { 4 }, { 4 }, ENeighborhood::Moore, 2 },
		{ TEXT("Generations 4/4/5/M"), { 4 }, { 4 }, ENeighborhood::Moore, 5 },
		{ TEXT("Generations 1-3/1-3/5/VN"), { 1, 2, 3 }, { 1, 2, 3 }, ENeighborhood::VonNeumann, 5 },
	};

	for (const FRuleCase& Case : Cases)
	{
		const FCellularAutomatonRule Rule(Case.Birth, Case.Survival, Case.Neighborhood, Case.States);

		FDenseCellGrid Source(CellSize, ChunkSize, Rule.HasDecayStates());
		AutomataTestUtils::SeedSphere(Source, /*Seed=*/1337, /*Radius=*/12, /*Amount=*/4000);

		const FCpuComputeStrategy CpuStrategy;
		const FGpuComputeStrategy GpuStrategy(VolumeLimit);

		// Несколько поколений подряд, а не одно: угасание проявляется только
		// со второго (сначала клеткам надо перестать выживать).
		TUniquePtr<FCellGrid> CpuGrid;
		TUniquePtr<FCellGrid> GpuGrid;
		const FCellGrid* CpuSource = &Source;
		const FCellGrid* GpuSource = &Source;

		for (int32 Generation = 0; Generation < 4; ++Generation)
		{
			TUniquePtr<FCellGrid> NextCpu = AutomataTestUtils::AdvanceOneGeneration(*CpuSource, CpuStrategy, Rule, CellSize, ChunkSize);
			TUniquePtr<FCellGrid> NextGpu = AutomataTestUtils::AdvanceOneGeneration(*GpuSource, GpuStrategy, Rule, CellSize, ChunkSize);

			FString Mismatch;
			if (!AutomataTestUtils::GridsMatch(*NextCpu, *NextGpu, Mismatch))
			{
				AddError(FString::Printf(TEXT("%s: поколение %d разошлось - %s"), Case.Name, Generation + 1, *Mismatch));
				break;
			}

			CpuGrid = MoveTemp(NextCpu);
			GpuGrid = MoveTemp(NextGpu);
			CpuSource = CpuGrid.Get();
			GpuSource = GpuGrid.Get();
		}

		if (CpuGrid.IsValid())
		{
			AddInfo(FString::Printf(TEXT("%s: %d живых клеток после 4 поколений"), Case.Name, CpuGrid->Num()));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGpuBatchParityTest,
	"CellularAutomata.Compute.GpuBatchParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FGpuBatchParityTest::RunTest(const FString& Parameters)
{
	if (!AutomataTestUtils::IsGpuComputeAvailable())
	{
		AddInfo(TEXT("GPU-compute недоступен (нет RHI либо compute-шейдеров) - тест пропущен"));
		return true;
	}

	constexpr float CellSize = 100.0f;
	constexpr int32 ChunkSize = 16;
	constexpr int64 VolumeLimit = 512LL * 512LL * 512LL;
	constexpr int32 BatchSize = 5;

	// Ровно то, что руками проверялось через MCP при вводе пачек: N поколений
	// одним кругом обязаны совпасть с N одиночными шагами - включая возрасты
	// (их внутри пачки ведёт шейдер, а не CellAging) и стадии угасания (их
	// тоже, вместо CellDecay).
	struct FRuleCase
	{
		const TCHAR* Name;
		TArray<int32> Birth;
		TArray<int32> Survival;
		int32 States;
	};

	const TArray<FRuleCase> Cases = {
		{ TEXT("бинарное 4/4/2/M"), { 4 }, { 4 }, 2 },
		{ TEXT("Generations 4/4/5/M"), { 4 }, { 4 }, 5 },
	};

	for (const FRuleCase& Case : Cases)
	{
		const FCellularAutomatonRule Rule(Case.Birth, Case.Survival, ENeighborhood::Moore, Case.States);
		const FGpuComputeStrategy GpuStrategy(VolumeLimit);

		FDenseCellGrid Source(CellSize, ChunkSize, Rule.HasDecayStates());
		AutomataTestUtils::SeedSphere(Source, /*Seed=*/2024, /*Radius=*/12, /*Amount=*/4000);

		// Эталон: BatchSize отдельных шагов, каждый со своими CPU-проходами.
		TUniquePtr<FCellGrid> StepByStep;
		const FCellGrid* StepSource = &Source;
		for (int32 Generation = 0; Generation < BatchSize; ++Generation)
		{
			TUniquePtr<FCellGrid> Next = AutomataTestUtils::AdvanceOneGeneration(*StepSource, GpuStrategy, Rule, CellSize, ChunkSize);
			StepByStep = MoveTemp(Next);
			StepSource = StepByStep.Get();
		}

		// Пачка: один вызов, никаких CPU-проходов после (стратегия обязана
		// была заполнить и возрасты, и угасание сама - см. её doc-comment).
		TUniquePtr<FCellGrid> Batched = MakeUnique<FDenseCellGrid>(CellSize, ChunkSize, Rule.HasDecayStates());
		const int32 Advanced = GpuStrategy.StepBatch(Source, *Batched, Rule, BatchSize);

		if (!TestEqual(FString::Printf(TEXT("%s: пачка продвинула все %d поколений"), Case.Name, BatchSize), Advanced, BatchSize))
		{
			// Урезанная пачка (объём не влез в лимит) - не ошибка сама по себе,
			// но сравнивать с эталоном из BatchSize шагов уже нельзя.
			AddWarning(FString::Printf(TEXT("%s: пачка урезана до %d - сравнение пропущено"), Case.Name, Advanced));
			continue;
		}

		FString Mismatch;
		if (!AutomataTestUtils::GridsMatch(*StepByStep, *Batched, Mismatch))
		{
			AddError(FString::Printf(TEXT("%s: пачка разошлась с пошаговым прогоном - %s"), Case.Name, *Mismatch));
			continue;
		}

		AddInfo(FString::Printf(TEXT("%s: %d живых клеток после %d поколений, пачка совпала"), Case.Name, Batched->Num(), BatchSize));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateGeneratorDeterminismTest,
	"CellularAutomata.Generation.Determinism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FStateGeneratorDeterminismTest::RunTest(const FString& Parameters)
{
	// Каждый тип перечисления - по разу: один и тот же Seed обязан дать
	// поэлементно тот же набор, другой Seed - другой. Второе не менее важно
	// первого: генератор, который просто игнорирует Seed, первую проверку
	// проходит идеально.
	const int32 TypeCount = static_cast<int32>(EStateGeneratorType::SymmetricSeed) + 1;

	for (int32 TypeIndex = 0; TypeIndex < TypeCount; ++TypeIndex)
	{
		const EStateGeneratorType Type = static_cast<EStateGeneratorType>(TypeIndex);

		FStateGeneratorParams Params;
		Params.Type = Type;
		// Область поменьше умолчаний - тест должен быть быстрым, а
		// детерминизм от размера не зависит.
		Params.Extent = FIntVector(12, 12, 12);
		Params.Radius = 8;
		Params.Amount = 200;
		Params.ClusterCount = 5;
		Params.ClusterRadius = 3;
		Params.CoreExtent = FIntVector(3, 3, 3);

		const FString Name = StateGenerators::GetDisplayName(Type);

		TArray<FIntVector> First;
		TArray<FIntVector> Second;
		TArray<FIntVector> Other;
		StateGenerators::FGenerateStats Stats;
		FString Error;

		if (!StateGenerators::Generate(Params, /*Seed=*/1234, MAX_int64, First, Stats, Error))
		{
			AddError(FString::Printf(TEXT("%s: генерация не удалась - %s"), *Name, *Error));
			continue;
		}

		if (!StateGenerators::Generate(Params, /*Seed=*/1234, MAX_int64, Second, Stats, Error))
		{
			AddError(FString::Printf(TEXT("%s: повторная генерация не удалась - %s"), *Name, *Error));
			continue;
		}

		if (First != Second)
		{
			AddError(FString::Printf(TEXT("%s: тот же сид дал другой набор (%d против %d клеток)"),
				*Name, First.Num(), Second.Num()));
			continue;
		}

		// У шума и затравки заполнение вероятностное, а у кластеров ещё и
		// перекрываются зёрна - там оценка честно объявлена ожидаемой, и
		// требовать от неё верхней границы нельзя. У остальных построение
		// детерминированное, и оценка обязана быть именно ВЕРХНЕЙ: на ней
		// стоит проверка бюджета, и занижение означало бы, что генератор
		// строит больше, чем разрешено (ровно так и всплыла ошибка в оценке
		// полой сферы - непрерывный объём вместо счёта решёточных точек).
		const bool bEstimateIsUpperBound =
			Type != EStateGeneratorType::NoiseUniform &&
			Type != EStateGeneratorType::NoisePerlin &&
			Type != EStateGeneratorType::NoiseClusters &&
			Type != EStateGeneratorType::SymmetricSeed;

		const int64 Estimate = StateGenerators::EstimateCellCount(Params);
		if (bEstimateIsUpperBound && Estimate < First.Num())
		{
			AddError(FString::Printf(TEXT("%s: оценка %lld ниже фактических %d клеток"),
				*Name, Estimate, First.Num()));
		}

		// У чисто детерминированных построений (решётки, тела) сид не значит
		// ничего по определению - для них проверять "другой сид даёт другое"
		// нечего.
		const bool bUsesSeed =
			Type == EStateGeneratorType::RandomBall ||
			Type == EStateGeneratorType::NoiseUniform ||
			Type == EStateGeneratorType::NoisePerlin ||
			Type == EStateGeneratorType::NoiseClusters ||
			Type == EStateGeneratorType::SymmetricSeed;

		if (!bUsesSeed)
		{
			AddInfo(FString::Printf(TEXT("%s: %d клеток, воспроизводимо"), *Name, First.Num()));
			continue;
		}

		if (!StateGenerators::Generate(Params, /*Seed=*/4321, MAX_int64, Other, Stats, Error))
		{
			AddError(FString::Printf(TEXT("%s: генерация с другим сидом не удалась - %s"), *Name, *Error));
			continue;
		}

		if (First == Other)
		{
			AddError(FString::Printf(TEXT("%s: другой сид дал ровно тот же набор - сид не используется"), *Name));
			continue;
		}

		AddInfo(FString::Printf(TEXT("%s: %d клеток, воспроизводимо и зависит от сида"), *Name, First.Num()));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLatticeNeighborUniformityTest,
	"CellularAutomata.Generation.LatticeNeighborUniformity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FLatticeNeighborUniformityTest::RunTest(const FString& Parameters)
{
	// Ради этого свойства решётчатые генераторы и существуют: если ВСЕ живые
	// клетки видят одно и то же число соседей, а примыкающие пустые - другое,
	// то правило можно подобрать так, чтобы структура стояла вечно, а выбитая
	// из неё одна клетка запускала цепную реакцию. Числа здесь - те самые, что
	// обещаны в подсказках пресетов; разъедься они с реальностью, подсказка
	// уводила бы в подбор заведомо невозможного правила.
	struct FCase
	{
		const TCHAR* Name;
		EStateGeneratorType Type;
		int32 ExpectedAliveNeighbors;
		int32 ForbiddenEmptyNeighbors;
	};

	static const FCase Cases[] = {
		// Плоскости толщиной в клетку: живая видит 8 своих соседей по плите,
		// а пустая прямо над плитой - все 9 клеток под собой.
		{ TEXT("плоскости"), EStateGeneratorType::LatticePlanes, 8, 9 },
		// Блоки 2x2x2: живая видит 7 соседей по блоку, а пустая - не больше 4.
		{ TEXT("блоки"),     EStateGeneratorType::LatticeBlocks, 7, 7 },
	};

	for (const FCase& Case : Cases)
	{
		FStateGeneratorParams Params;
		Params.Type = Case.Type;
		Params.Extent = FIntVector(40, 40, 40);
		Params.Period = FIntVector(8, 8, 8);
		Params.Thickness = 1;
		Params.BlockSize = 2;
		Params.bAxisX = true;
		Params.bAxisY = false;
		Params.bAxisZ = false;

		TArray<FIntVector> Cells;
		StateGenerators::FGenerateStats Stats;
		FString Error;

		if (!StateGenerators::Generate(Params, /*Seed=*/0, MAX_int64, Cells, Stats, Error))
		{
			AddError(FString::Printf(TEXT("%s: генерация не удалась - %s"), Case.Name, *Error));
			continue;
		}

		StateGenerators::FNeighborHistogram Histogram;
		// Полуразмер выборки заметно меньше области построения - иначе в неё
		// попали бы клетки у самого края, у которых соседей меньше просто
		// потому, что структура там кончается.
		StateGenerators::AnalyzeNeighborCounts(Cells, ENeighborhood::Moore, /*MaxSampleExtent=*/20, Histogram);

		if (Histogram.SampledAlive == 0)
		{
			AddError(FString::Printf(TEXT("%s: в выборку не попало ни одной живой клетки"), Case.Name));
			continue;
		}

		// Однородность: вся масса гистограммы обязана стоять в одной колонке.
		const int64 AtExpected = Histogram.AliveByCount[Case.ExpectedAliveNeighbors];
		if (AtExpected != Histogram.SampledAlive)
		{
			AddError(FString::Printf(TEXT("%s: соседей по %d только у %lld из %lld живых клеток - %s"),
				Case.Name, Case.ExpectedAliveNeighbors, AtExpected, Histogram.SampledAlive,
				*StateGenerators::DescribeHistogram(Histogram)));
			continue;
		}

		// И ни одна примыкающая пустая клетка не должна попадать в то же
		// число: иначе "выживает" и "рождается" неразделимы, и структура
		// принципиально не может быть метастабильной.
		const int64 EmptyClash = Histogram.EmptyByCount[Case.ForbiddenEmptyNeighbors];
		if (Case.ForbiddenEmptyNeighbors == Case.ExpectedAliveNeighbors && EmptyClash > 0)
		{
			AddError(FString::Printf(TEXT("%s: %lld пустых клеток видят столько же соседей (%d), сколько живые - %s"),
				Case.Name, EmptyClash, Case.ForbiddenEmptyNeighbors,
				*StateGenerators::DescribeHistogram(Histogram)));
			continue;
		}

		AddInfo(FString::Printf(TEXT("%s: все %lld живых клеток видят ровно %d соседей - %s"),
			Case.Name, Histogram.SampledAlive, Case.ExpectedAliveNeighbors,
			*StateGenerators::DescribeHistogram(Histogram)));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRandomBallLegacyParityTest,
	"CellularAutomata.Generation.RandomBallParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FRandomBallLegacyParityTest::RunTest(const FString& Parameters)
{
	// Золотой тест: генератор RandomBall обязан давать ровно то же, что давал
	// прежний цикл внутри GenerateRandom(). Эталон - AutomataTestUtils::
	// SeedSphere(), который этот цикл и воспроизводит. Без такой проверки
	// рефакторинг мог бы тихо сдвинуть все ранее сохранённые сиды: одна и та
	// же цифра в поле Random Seed начала бы давать другую картинку.
	struct FCase
	{
		int32 Seed;
		int32 Radius;
		int32 Amount;
	};

	static const FCase Cases[] = {
		{ 0,    10, 1000 },
		{ 1234, 20, 5000 },
		{ -7,    5,  300 },
	};

	for (const FCase& Case : Cases)
	{
		FDenseCellGrid Reference(100.0f, 16);
		AutomataTestUtils::SeedSphere(Reference, Case.Seed, Case.Radius, Case.Amount);

		FStateGeneratorParams Params;
		Params.Type = EStateGeneratorType::RandomBall;
		Params.Radius = Case.Radius;
		Params.Amount = Case.Amount;

		TArray<FIntVector> Cells;
		StateGenerators::FGenerateStats Stats;
		FString Error;

		if (!StateGenerators::Generate(Params, Case.Seed, MAX_int64, Cells, Stats, Error))
		{
			AddError(FString::Printf(TEXT("сид %d: генерация не удалась - %s"), Case.Seed, *Error));
			continue;
		}

		// Генератор отдаёт броски как есть, с повторами (их поглощает заливка
		// в сетку), поэтому сравнивать надо осевшие клетки, а не длины
		// массивов.
		FDenseCellGrid Produced(100.0f, 16);
		for (const FIntVector& Cell : Cells)
		{
			Produced.SetAlive(Cell, true);
		}

		FString Mismatch;
		if (!AutomataTestUtils::GridsMatch(Reference, Produced, Mismatch))
		{
			AddError(FString::Printf(TEXT("сид %d: генератор разошёлся с прежним GenerateRandom() - %s"),
				Case.Seed, *Mismatch));
			continue;
		}

		AddInfo(FString::Printf(TEXT("сид %d: %d клеток, совпало с прежним поведением"), Case.Seed, Produced.Num()));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSliceCaptureOrientationTest,
	"CellularAutomata.Slice.Orientation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FSliceCaptureOrientationTest::RunTest(const FString& Parameters)
{
	// Фигура АСИММЕТРИЧНАЯ намеренно: на симметричной не видно ни
	// транспонирования, ни зеркала - то есть ровно тех ошибок, ради которых
	// тест и пишется. Буква "Г" в плоскости XY на высоте 0:
	//   (0,0) угол, (2,0) конец горизонтали, (0,1) конец вертикали.
	constexpr double CellSize = 100.0;

	auto MakeCell = [](double X, double Y, double Z, FColor Color)
	{
		FCellRenderInstance Instance;
		Instance.Position = FVector3f(static_cast<float>(X * CellSize), static_cast<float>(Y * CellSize), static_cast<float>(Z * CellSize));
		Instance.Color = Color;
		return Instance;
	};

	const FColor Corner(255, 0, 0, 255);
	const FColor AlongX(0, 255, 0, 255);
	const FColor AlongY(0, 0, 255, 255);

	TArray<FCellRenderInstance> Cells = {
		MakeCell(0, 0, 0, Corner),
		MakeCell(2, 0, 0, AlongX),
		MakeCell(0, 1, 0, AlongY),
	};

	// Взгляд сверху вниз, "север" камеры направлен по +X - то же, что даёт
	// NumPad8 при нулевом рыскании.
	CellRasterizer::FRasterParams Params;
	Params.CellSize = CellSize;
	Params.PixelsPerCell = 1;
	Params.BackgroundColor = FColor(0, 0, 0, 255);
	CellRasterizer::BuildAxes(/*CameraForward=*/FVector(0, 0, -1), /*CameraUp=*/FVector(1, 0, 0), Params);

	CellRasterizer::FRasterImage Image;
	FString Error;
	if (!CellRasterizer::Rasterize(Cells, Params, MAX_int64, Image, Error))
	{
		AddError(FString::Printf(TEXT("растеризация не удалась - %s"), *Error));
		return true;
	}

	// Три клетки: 3 вдоль X (0..2) и 2 вдоль Y (0..1). Сверху ось X идёт
	// вверх по картинке, значит высота 3, ширина 2.
	TestEqual(TEXT("ширина"), Image.Width, 2);
	TestEqual(TEXT("высота"), Image.Height, 3);

	auto PixelAt = [&Image](int32 X, int32 Y) -> FColor
	{
		return Image.Pixels[Y * Image.Width + X];
	};

	// Угол лежит внизу картинки (X = 0 это "юг" при взгляде сверху с севером
	// по +X), клетка с большим X - вверху, клетка с большим Y - правее.
	const FColor CornerPixel = PixelAt(0, 2);
	const FColor AlongXPixel = PixelAt(0, 0);
	const FColor AlongYPixel = PixelAt(1, 2);

	TestEqual(TEXT("угол фигуры"), CornerPixel.ToPackedARGB(), Corner.ToPackedARGB());
	TestEqual(TEXT("конец вдоль X - вверху"), AlongXPixel.ToPackedARGB(), AlongX.ToPackedARGB());
	TestEqual(TEXT("конец вдоль Y - справа"), AlongYPixel.ToPackedARGB(), AlongY.ToPackedARGB());

	// Оси обязаны остаться попарно ортогональными и осевыми.
	TestTrue(TEXT("оси ортогональны"),
		FMath::IsNearlyZero(FVector::DotProduct(Params.RightAxis, Params.UpAxis)) &&
		FMath::IsNearlyZero(FVector::DotProduct(Params.RightAxis, Params.ForwardAxis)) &&
		FMath::IsNearlyZero(FVector::DotProduct(Params.UpAxis, Params.ForwardAxis)));

	// Диагональный ракурс не должен схлопывать оси в одну - это и есть тот
	// случай, ради которого горизонталь выводится через векторное
	// произведение, а не снапается отдельно.
	CellRasterizer::FRasterParams Diagonal;
	CellRasterizer::BuildAxes(FVector(1, 1, -1).GetSafeNormal(), FVector(0, 0, 1), Diagonal);
	TestTrue(TEXT("диагональ: оси не совпали"),
		!Diagonal.ForwardAxis.Equals(Diagonal.UpAxis) &&
		!Diagonal.RightAxis.IsNearlyZero());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSliceCaptureRasterTest,
	"CellularAutomata.Slice.Raster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FSliceCaptureRasterTest::RunTest(const FString& Parameters)
{
	constexpr double CellSize = 100.0;

	auto MakeCell = [](double X, double Y, double Z, FColor Color)
	{
		FCellRenderInstance Instance;
		Instance.Position = FVector3f(static_cast<float>(X * CellSize), static_cast<float>(Y * CellSize), static_cast<float>(Z * CellSize));
		Instance.Color = Color;
		return Instance;
	};

	CellRasterizer::FRasterParams Params;
	Params.CellSize = CellSize;
	Params.BackgroundColor = FColor(0, 0, 0, 255);
	CellRasterizer::BuildAxes(FVector(0, 0, -1), FVector(1, 0, 0), Params);

	const FColor Near(10, 200, 30, 255);
	const FColor Far(200, 10, 30, 255);

	// Две клетки на одном луче: камера смотрит вниз (-Z), значит ближняя - та,
	// что ВЫШЕ. Побеждать обязана она, причём независимо от порядка во входном
	// массиве: порядок GetAliveCells() меняется от поколения к поколению.
	{
		TArray<FCellRenderInstance> Straight = { MakeCell(0, 0, 5, Near), MakeCell(0, 0, 0, Far) };
		TArray<FCellRenderInstance> Reversed = { MakeCell(0, 0, 0, Far), MakeCell(0, 0, 5, Near) };

		CellRasterizer::FRasterImage ImageA;
		CellRasterizer::FRasterImage ImageB;
		FString Error;

		if (!CellRasterizer::Rasterize(Straight, Params, MAX_int64, ImageA, Error) ||
			!CellRasterizer::Rasterize(Reversed, Params, MAX_int64, ImageB, Error))
		{
			AddError(FString::Printf(TEXT("растеризация не удалась - %s"), *Error));
			return true;
		}

		TestEqual(TEXT("один пиксель"), ImageA.Pixels.Num(), 1);
		TestEqual(TEXT("побеждает ближняя клетка"), ImageA.Pixels[0].ToPackedARGB(), Near.ToPackedARGB());
		TestEqual(TEXT("порядок входа не влияет"), ImageB.Pixels[0].ToPackedARGB(), ImageA.Pixels[0].ToPackedARGB());
	}

	// Масштаб: одна клетка при PixelsPerCell = 4 обязана дать ровный блок 4x4
	// одного цвета - никакой интерполяции, ни одного промежуточного оттенка.
	{
		CellRasterizer::FRasterParams Scaled = Params;
		Scaled.PixelsPerCell = 4;

		TArray<FCellRenderInstance> One = { MakeCell(0, 0, 0, Near) };
		CellRasterizer::FRasterImage Image;
		FString Error;

		if (!CellRasterizer::Rasterize(One, Scaled, MAX_int64, Image, Error))
		{
			AddError(FString::Printf(TEXT("растеризация не удалась - %s"), *Error));
			return true;
		}

		TestEqual(TEXT("ширина блока"), Image.Width, 4);
		TestEqual(TEXT("высота блока"), Image.Height, 4);

		bool bUniform = true;
		for (const FColor& Pixel : Image.Pixels)
		{
			bUniform = bUniform && (Pixel.ToPackedARGB() == Near.ToPackedARGB());
		}
		TestTrue(TEXT("весь блок одного цвета"), bUniform);
	}

	// Бюджет: отказ обязан прийти ДО построения и не оставить полурезультата.
	{
		TArray<FCellRenderInstance> FarApart = { MakeCell(0, 0, 0, Near), MakeCell(5000, 5000, 0, Far) };

		CellRasterizer::FRasterImage Image;
		FString Error;
		const bool bOk = CellRasterizer::Rasterize(FarApart, Params, /*MaxPixels=*/1024, Image, Error);

		TestFalse(TEXT("превышение бюджета отклонено"), bOk);
		TestTrue(TEXT("причина названа"), !Error.IsEmpty());
		TestEqual(TEXT("буфер не тронут"), Image.Pixels.Num(), 0);
	}

	// Пустой вход - честная ошибка, а не картинка 1x1.
	{
		TArray<FCellRenderInstance> Empty;
		CellRasterizer::FRasterImage Image;
		FString Error;

		TestFalse(TEXT("пустой вход отклонён"), CellRasterizer::Rasterize(Empty, Params, MAX_int64, Image, Error));
		TestTrue(TEXT("причина названа"), !Error.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSliceTileTest,
	"CellularAutomata.Slice.Tile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FSliceTileTest::RunTest(const FString& Parameters)
{
	// Полоса из четырёх различимых цветов: на однотонной картинке отражение
	// выглядит правильным всегда, даже когда оно неправильное.
	auto MakeStrip = [](int32 Width) -> CellRasterizer::FRasterImage
	{
		CellRasterizer::FRasterImage Image;
		Image.Width = Width;
		Image.Height = 1;
		Image.Pixels.SetNumUninitialized(Width);
		for (int32 X = 0; X < Width; ++X)
		{
			Image.Pixels[X] = FColor(static_cast<uint8>(X * 40 + 10), 0, 0, 255);
		}
		return Image;
	};

	// Отражение по горизонтали: [0 1 2 3] -> [0 1 2 3 2 1], ширина 2W-2.
	{
		CellRasterizer::FRasterImage Image = MakeStrip(4);
		const FColor C0 = Image.Pixels[0];
		const FColor C1 = Image.Pixels[1];
		const FColor C2 = Image.Pixels[2];
		const FColor C3 = Image.Pixels[3];

		CellRasterizer::MakeTile(Image, /*bMirrorX=*/true, /*bMirrorY=*/false);

		TestEqual(TEXT("ширина тайла 2W-2"), Image.Width, 6);

		TestEqual(TEXT("столбец 0"), Image.Pixels[0].ToPackedARGB(), C0.ToPackedARGB());
		TestEqual(TEXT("столбец 3"), Image.Pixels[3].ToPackedARGB(), C3.ToPackedARGB());
		// Ключевое: сразу за крайним столбцом идёт ПРЕДпоследний, а не его
		// повтор - иначе на каждом шве была бы двойная линия.
		TestEqual(TEXT("за краем идёт предпоследний, а не повтор края"),
			Image.Pixels[4].ToPackedARGB(), C2.ToPackedARGB());
		TestEqual(TEXT("последний столбец"), Image.Pixels[5].ToPackedARGB(), C1.ToPackedARGB());

		// Замыкание тайла на себя: за последним столбцом следует столбец 0
		// следующей копии, и эта пара обязана быть так же симметрична, как
		// внутри тайла - то есть сосед края с обеих сторон один и тот же.
		TestEqual(TEXT("тайл замкнут: соседи столбца 0 совпадают"),
			Image.Pixels[Image.Width - 1].ToPackedARGB(), Image.Pixels[1].ToPackedARGB());
	}

	// По вертикали - то же самое на транспонированной картинке.
	{
		CellRasterizer::FRasterImage Image;
		Image.Width = 1;
		Image.Height = 4;
		Image.Pixels.SetNumUninitialized(4);
		for (int32 Y = 0; Y < 4; ++Y)
		{
			Image.Pixels[Y] = FColor(0, static_cast<uint8>(Y * 40 + 10), 0, 255);
		}
		const FColor R1 = Image.Pixels[1];
		const FColor R2 = Image.Pixels[2];

		CellRasterizer::MakeTile(Image, /*bMirrorX=*/false, /*bMirrorY=*/true);

		TestEqual(TEXT("высота тайла 2H-2"), Image.Height, 6);
		TestEqual(TEXT("строка за краем"), Image.Pixels[4].ToPackedARGB(), R2.ToPackedARGB());
		TestEqual(TEXT("последняя строка"), Image.Pixels[5].ToPackedARGB(), R1.ToPackedARGB());
	}

	// Отражать нечего - картинка в один пиксель по оси обязана остаться собой,
	// а не выродиться в нулевой размер (2*1-2 == 0).
	{
		CellRasterizer::FRasterImage Image = MakeStrip(1);
		CellRasterizer::MakeTile(Image, /*bMirrorX=*/true, /*bMirrorY=*/true);
		TestEqual(TEXT("ширина не выродилась"), Image.Width, 1);
		TestEqual(TEXT("высота не выродилась"), Image.Height, 1);
		TestEqual(TEXT("пиксель на месте"), Image.Pixels.Num(), 1);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
