#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

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

#endif // WITH_DEV_AUTOMATION_TESTS
