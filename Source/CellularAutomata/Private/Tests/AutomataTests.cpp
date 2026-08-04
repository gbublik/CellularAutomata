#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Automata/Capture/CellRasterizer.h"
#include "Automata/Generation/StateGenerators.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Grid/LatticeTransform.h"
#include "Automata/Meshing/ChunkGridView.h"
#include "Automata/Rendering/ColorRamp.h"
#include "Automata/Simulation/LatticeNeighborhood.h"
#include "Automata/Simulation/CellAging.h"
#include "Automata/Simulation/CellDecay.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/RuleStringParser.h"
#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"
#include "Automata/Simulation/ComputeStrategy/GpuComputeStrategy.h"
#include "Math/RandomStream.h"
#include "Orchestration/GenerationHistory.h"
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
 *    (см. FGpuComputeStrategy::StepBatch());
 *  - скользящее окно графика поколений (namespace GenerationHistory) - ради
 *    него вся эта логика и вынесена из оркестратора свободными функциями:
 *    актора, тика и рендера она не требует, а ловушек в ней хватает
 *    (правка последнего замера на месте, перенос значения вперёд, раскладка
 *    по значению поколения вместо индекса).
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNeighborhoodShellsTest,
	"CellularAutomata.Rules.NeighborhoodShells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FNeighborhoodShellsTest::RunTest(const FString& Parameters)
{
	// Четыре оболочки - вся модель соседства целиком (см. ENeighborhood).
	// Проверяется она сама, а не отдельные значения: если оболочки верны и
	// значения из них собраны, то верно и всё остальное.
	const TArray<FIntVector> Faces = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::VonNeumann);
	const TArray<FIntVector> Edges = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::Edges);
	const TArray<FIntVector> Corners = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::Corners);
	const TArray<FIntVector> FarAxes = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::FarAxes);

	TestEqual(TEXT("грани - 6"), Faces.Num(), 6);
	TestEqual(TEXT("рёбра - 12"), Edges.Num(), 12);
	TestEqual(TEXT("диагонали - 8"), Corners.Num(), 8);
	TestEqual(TEXT("дальние оси - 6"), FarAxes.Num(), 6);

	// Каждая оболочка - ровно один класс по d^2, без примесей. Куб 5x5x5, по
	// которому идёт перебор, полон смещений вроде (2,1,0) с d^2=5, и они не
	// должны просачиваться никуда.
	auto CheckShell = [this](const TArray<FIntVector>& Offsets, int32 ExpectedDistSq, const TCHAR* Label)
	{
		bool bOk = true;
		for (const FIntVector& Offset : Offsets)
		{
			if (Offset.X * Offset.X + Offset.Y * Offset.Y + Offset.Z * Offset.Z != ExpectedDistSq)
			{
				bOk = false;
			}
		}
		TestTrue(FString::Printf(TEXT("%s: все смещения одного класса d^2"), Label), bOk);
	};
	CheckShell(Faces, 1, TEXT("грани"));
	CheckShell(Edges, 2, TEXT("рёбра"));
	CheckShell(Corners, 3, TEXT("диагонали"));
	CheckShell(FarAxes, 4, TEXT("дальние оси"));
	TestFalse(TEXT("дальние оси не содержат (2,1,0)"), FarAxes.Contains(FIntVector(2, 1, 0)));
	TestFalse(TEXT("дальние оси не содержат (2,2,0)"), FarAxes.Contains(FIntVector(2, 2, 0)));

	// Все 14 значений: размер, отсутствие повторов, и то, что набор в точности
	// равен объединению своих оболочек. Последнее - главное: числа сошлись бы и
	// при перепутанных классах (6+8 и 12+2 одинаково дают 14).
	struct FCase
	{
		ENeighborhood Value;
		int32 ExpectedCount;
		bool bFaces;
		bool bEdges;
		bool bCorners;
		bool bFarAxes;
		const TCHAR* Label;
	};

	const TArray<FCase> Cases = {
		{ ENeighborhood::VonNeumann,           6, true,  false, false, false, TEXT("VonNeumann") },
		{ ENeighborhood::Moore,               26, true,  true,  true,  false, TEXT("Moore") },
		{ ENeighborhood::VonNeumann2,         24, true,  true,  false, true,  TEXT("VonNeumann2") },
		{ ENeighborhood::Edges,               12, false, true,  false, false, TEXT("Edges") },
		{ ENeighborhood::Corners,              8, false, false, true,  false, TEXT("Corners") },
		{ ENeighborhood::FarAxes,              6, false, false, false, true,  TEXT("FarAxes") },
		{ ENeighborhood::FacesEdges,          18, true,  true,  false, false, TEXT("FacesEdges") },
		{ ENeighborhood::FacesCorners,        14, true,  false, true,  false, TEXT("FacesCorners") },
		{ ENeighborhood::FacesFarAxes,        12, true,  false, false, true,  TEXT("FacesFarAxes") },
		{ ENeighborhood::EdgesCorners,        20, false, true,  true,  false, TEXT("EdgesCorners") },
		{ ENeighborhood::EdgesFarAxes,        18, false, true,  false, true,  TEXT("EdgesFarAxes") },
		{ ENeighborhood::CornersFarAxes,      14, false, false, true,  true,  TEXT("CornersFarAxes") },
		{ ENeighborhood::FacesCornersFarAxes, 20, true,  false, true,  true,  TEXT("FacesCornersFarAxes") },
		{ ENeighborhood::EdgesCornersFarAxes, 26, false, true,  true,  true,  TEXT("EdgesCornersFarAxes") },
	};

	for (const FCase& Case : Cases)
	{
		const TArray<FIntVector> Offsets = FCellularAutomatonRule::BuildNeighborOffsets(Case.Value);
		TestEqual(FString::Printf(TEXT("%s: число соседей"), Case.Label), Offsets.Num(), Case.ExpectedCount);

		TSet<FIntVector> Expected;
		if (Case.bFaces)   { Expected.Append(Faces); }
		if (Case.bEdges)   { Expected.Append(Edges); }
		if (Case.bCorners) { Expected.Append(Corners); }
		if (Case.bFarAxes) { Expected.Append(FarAxes); }

		TestEqual(FString::Printf(TEXT("%s: без повторов"), Case.Label), TSet<FIntVector>(Offsets).Num(), Offsets.Num());
		TestEqual(FString::Printf(TEXT("%s: размер совпал с объединением оболочек"), Case.Label), Expected.Num(), Offsets.Num());

		bool bMatches = true;
		for (const FIntVector& Offset : Offsets)
		{
			if (!Expected.Contains(Offset))
			{
				bMatches = false;
			}
		}
		TestTrue(FString::Printf(TEXT("%s: набор равен объединению своих оболочек"), Case.Label), bMatches);

		// Потолок шейдерного массива - он же потолок 32-битных масок правила
		// (счётчик соседей не может превысить их число). Таблица обязана
		// оставаться под ним целиком.
		TestTrue(FString::Printf(TEXT("%s: влезает в шейдерный массив"), Case.Label), Offsets.Num() <= 26);
	}

	// Moore обязан отдавать прежние 26 смещений в прежнем порядке: порядок ни
	// на что не влияет (оба compute-пути только суммируют), но сохранить его
	// дешевле, чем каждый раз доказывать, что можно не сохранять.
	{
		TArray<FIntVector> ExpectedMoore;
		for (int32 dx = -1; dx <= 1; ++dx)
		{
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				for (int32 dz = -1; dz <= 1; ++dz)
				{
					if (dx != 0 || dy != 0 || dz != 0)
					{
						ExpectedMoore.Add(FIntVector(dx, dy, dz));
					}
				}
			}
		}
		TestTrue(TEXT("Moore - прежний набор в прежнем порядке"),
			FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::Moore) == ExpectedMoore);
	}

	// Структурные свойства, ради которых наборы без граней и существуют: рёбра
	// сохраняют чётность суммы координат (две подрешётки), диагонали
	// переворачивают чётность каждой координаты (четыре), дальние оси её
	// сохраняют (восемь). Сломайся это - наборы станут просто "Moore поменьше".
	for (const FIntVector& Offset : Edges)
	{
		TestTrue(TEXT("ребро сохраняет чётность суммы координат"), ((Offset.X + Offset.Y + Offset.Z) % 2) == 0);
	}
	for (const FIntVector& Offset : Corners)
	{
		TestTrue(TEXT("диагональ переворачивает чётность каждой координаты"),
			FMath::Abs(Offset.X) == 1 && FMath::Abs(Offset.Y) == 1 && FMath::Abs(Offset.Z) == 1);
	}
	for (const FIntVector& Offset : FarAxes)
	{
		TestTrue(TEXT("дальняя ось сохраняет чётность каждой координаты"),
			(Offset.X % 2) == 0 && (Offset.Y % 2) == 0 && (Offset.Z % 2) == 0);
	}

	// Размах - то, из чего считается гало GPU-пачки, и он обязан браться из
	// офсетов, а не из чего-либо ещё: наборы с дальними осями дотягиваются до
	// второй клетки, оставаясь обычными наборами оболочек.
	{
		const FCellularAutomatonRule MooreRule({ 1 }, { 2 }, ENeighborhood::Moore, 2);
		const FCellularAutomatonRule FarRule({ 1 }, { 2 }, ENeighborhood::FarAxes, 2);
		const FCellularAutomatonRule Vn2Rule({ 1 }, { 2 }, ENeighborhood::VonNeumann2, 2);

		TestEqual(TEXT("размах Moore"), MooreRule.GetNeighborExtent(), 1);
		TestEqual(TEXT("размах дальних осей"), FarRule.GetNeighborExtent(), 2);
		TestEqual(TEXT("размах VonNeumann2"), Vn2Rule.GetNeighborExtent(), 2);
		TestEqual(TEXT("правило построило офсеты по соседству"), Vn2Rule.GetNeighborOffsets().Num(), 24);
	}

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

	// "VN2" - самостоятельное ИМЯ, а не "VN с цифрой". Отдельным блоком,
	// потому что раньше хвостовые цифры отрезались как радиус, и такая строка
	// разбиралась бы как VonNeumann. Радиуса больше нет (см. Neighborhood.h),
	// имя сравнивается целиком.
	{
		RuleStringParser::FParsedRule Parsed;
		FString Error;
		TestTrue(TEXT("'0-6/1,3/2/VN2' разбирается"), RuleStringParser::ParseRuleString(TEXT("0-6/1,3/2/VN2"), Parsed, Error));
		TestTrue(TEXT("'VN2' - это VonNeumann2, а не VonNeumann"), Parsed.Neighborhood == ENeighborhood::VonNeumann2);

		TestTrue(TEXT("'0-6/1,3/2/VonNeumann' разбирается"), RuleStringParser::ParseRuleString(TEXT("0-6/1,3/2/VonNeumann"), Parsed, Error));
		TestTrue(TEXT("длинное имя - обычный VonNeumann"), Parsed.Neighborhood == ENeighborhood::VonNeumann);

		TestTrue(TEXT("'VonNeumann2' разбирается"), RuleStringParser::ParseRuleString(TEXT("0-6/1,3/2/VonNeumann2"), Parsed, Error));
		TestTrue(TEXT("'VonNeumann2' - это VonNeumann2"), Parsed.Neighborhood == ENeighborhood::VonNeumann2);
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
		TEXT("0-6/1,3/2/VN2"),
		TEXT("0-6/1,3/2/E"),
		TEXT("0-4/1,3/2/C"),
		TEXT("0-9/1,3/2/FE"),
		TEXT("0-3/1,3/2/FA"),
		TEXT("0-9/1,3/2/EFA"),
		TEXT("0-7/1,3/2/CFA"),
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

	// Каждое значение перечисления обязано печататься СВОИМ токеном и
	// разбираться обратно в себя же. Проверяется весь список разом: пропуск в
	// таблице токенов иначе тихо превратился бы в "M" (её fallback).
	{
		const TArray<TPair<ENeighborhood, FString>> Tokens = {
			{ ENeighborhood::VonNeumann,          TEXT("VN") },
			{ ENeighborhood::Moore,               TEXT("M") },
			{ ENeighborhood::VonNeumann2,         TEXT("VN2") },
			{ ENeighborhood::Edges,               TEXT("E") },
			{ ENeighborhood::Corners,             TEXT("C") },
			{ ENeighborhood::FarAxes,             TEXT("FA") },
			{ ENeighborhood::FacesEdges,          TEXT("FE") },
			{ ENeighborhood::FacesCorners,        TEXT("FC") },
			{ ENeighborhood::FacesFarAxes,        TEXT("FFA") },
			{ ENeighborhood::EdgesCorners,        TEXT("EC") },
			{ ENeighborhood::EdgesFarAxes,        TEXT("EFA") },
			{ ENeighborhood::CornersFarAxes,      TEXT("CFA") },
			{ ENeighborhood::FacesCornersFarAxes, TEXT("FCFA") },
			{ ENeighborhood::EdgesCornersFarAxes, TEXT("ECFA") },
		};

		for (const TPair<ENeighborhood, FString>& Token : Tokens)
		{
			const FString Expected = FString::Printf(TEXT("5/4/2/%s"), *Token.Value);
			TestEqual(FString::Printf(TEXT("токен '%s' печатается"), *Token.Value),
				RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, Token.Key), Expected);

			RuleStringParser::FParsedRule Parsed;
			FString Error;
			if (TestTrue(FString::Printf(TEXT("токен '%s' разбирается"), *Token.Value),
				RuleStringParser::ParseRuleString(Expected, Parsed, Error)))
			{
				TestTrue(FString::Printf(TEXT("токен '%s' разбирается в себя же"), *Token.Value),
					Parsed.Neighborhood == Token.Key);
			}
		}
	}

	// Точечные проверки нескольких токенов - те же, что выше, но читаемые в
	// отчёте по имени.
	TestEqual(TEXT("токен рёбер"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::Edges), FString(TEXT("5/4/2/E")));
	TestEqual(TEXT("токен диагоналей"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::Corners), FString(TEXT("5/4/2/C")));
	TestEqual(TEXT("токен граней+рёбер"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::FacesEdges), FString(TEXT("5/4/2/FE")));
	TestEqual(TEXT("токен дальних осей"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::FarAxes), FString(TEXT("5/4/2/FA")));
	TestEqual(TEXT("токен рёбер+дальних осей"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::EdgesFarAxes), FString(TEXT("5/4/2/EFA")));
	TestEqual(TEXT("токен диагоналей+дальних осей"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::CornersFarAxes), FString(TEXT("5/4/2/CFA")));

	// Длинные формы имён разбираются наравне с короткими.
	{
		RuleStringParser::FParsedRule Parsed;
		FString Error;
		TestTrue(TEXT("'FacesEdges' разбирается"), RuleStringParser::ParseRuleString(TEXT("1/2/2/FacesEdges"), Parsed, Error));
		TestTrue(TEXT("'FacesEdges' - это грани+рёбра"), Parsed.Neighborhood == ENeighborhood::FacesEdges);

		TestTrue(TEXT("'corners' разбирается регистронезависимо"), RuleStringParser::ParseRuleString(TEXT("1/2/2/corners"), Parsed, Error));
		TestTrue(TEXT("'corners' - это диагонали"), Parsed.Neighborhood == ENeighborhood::Corners);
	}

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
		// Соседства с цифрой больше не разбираются "почти как VN": имя
		// сравнивается целиком, поэтому всё, чего нет в таблице токенов, -
		// ошибка, а не VN какого-то радиуса.
		TEXT("1/2/2/VN0"),
		TEXT("1/2/2/VN3"),
		TEXT("1/2/2/M2"),
		TEXT("1/2/2/E2"),
		TEXT("1/2/2/FE2"),
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
		// Радиус 2: единственная автоматическая защита от неверно посчитанного
		// гало (пограничные клетки терялись бы молча) и от счётчиков соседей,
		// не влезающих в 32-битные маски. Правило подобрано под плотность
		// затравки (~10 живых соседей из 24), чтобы за 4 поколения ничего не
		// вымерло - на пустой сетке любые две реализации совпадают.
		{ TEXT("бинарное 6-16/12-13/2/VN2"), { 12, 13 }, { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, ENeighborhood::VonNeumann2, 2 },
		{ TEXT("Generations 6-16/12-13/5/VN2"), { 12, 13 }, { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, ENeighborhood::VonNeumann2, 5 },
		// Формы: набор офсетов перестаёт быть "шаром вокруг клетки", и обе
		// стратегии обязаны это одинаково пережить. Рёбра дополнительно
		// интересны тем, что решётка распадается на две независимые
		// подрешётки - расхождение проявилось бы как перетекание между ними.
		{ TEXT("бинарное 3-8/5-6/2/E"), { 5, 6 }, { 3, 4, 5, 6, 7, 8 }, ENeighborhood::Edges, 2 },
		{ TEXT("бинарное 5-11/8-9/2/FE"), { 8, 9 }, { 5, 6, 7, 8, 9, 10, 11 }, ENeighborhood::FacesEdges, 2 },
		// Формы с дальними осями: смещения дотягиваются до ВТОРОЙ клетки при
		// номинальном радиусе 1. Кандидаты CPU-стратегии и гало GPU обязаны
		// одинаково это учесть - расхождение здесь означало бы, что где-то
		// размах офсетов подменили радиусом.
		{ TEXT("бинарное 0-6/1,3/2/FA"), { 1, 3 }, { 0, 1, 2, 3, 4, 5, 6 }, ENeighborhood::FarAxes, 2 },
		{ TEXT("бинарное 0-14/1,3/2/CFA"), { 1, 3 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 }, ENeighborhood::CornersFarAxes, 2 },
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
			// Совпадение на вымершей сетке ничего не доказывает - две пустые
			// сетки равны при любой ошибке в подсчёте соседей.
			TestTrue(FString::Printf(TEXT("%s: сетка не вымерла (иначе сравнение пустое)"), Case.Name), CpuGrid->Num() > 0);
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
		ENeighborhood Neighborhood = ENeighborhood::Moore;
	};

	const TArray<FRuleCase> Cases = {
		{ TEXT("бинарное 4/4/2/M"), { 4 }, { 4 }, 2 },
		{ TEXT("Generations 4/4/5/M"), { 4 }, { 4 }, 5 },
		// Радиус 2 именно здесь важнее всего: гало пачки равно радиус*поколений,
		// то есть при радиусе 2 оно вдвое больше, чем было. Ошибка в этом
		// множителе не роняет ничего - она молча теряет пограничные клетки, и
		// расхождение с пошаговым прогоном единственное, что её показывает.
		{ TEXT("бинарное 6-16/12-13/2/VN2"), { 12, 13 }, { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, 2, ENeighborhood::VonNeumann2 },
		// Самый ценный случай во всём файле: форма с дальними осями имеет
		// РАДИУС 1, но РАЗМАХ 2, поэтому пачке из 5 поколений нужно гало 10,
		// а не 5. Если гало где-нибудь снова начнут считать от радиуса, пачка
		// потеряет пограничные клетки - молча, без падения и без строчки в
		// логе, и разойдётся с пошаговым прогоном только здесь.
		{ TEXT("бинарное 0-6/1,3/2/FA"), { 1, 3 }, { 0, 1, 2, 3, 4, 5, 6 }, 2, ENeighborhood::FarAxes },
	};

	for (const FRuleCase& Case : Cases)
	{
		const FCellularAutomatonRule Rule(Case.Birth, Case.Survival, Case.Neighborhood, Case.States);
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
		TestTrue(FString::Printf(TEXT("%s: сетка не вымерла (иначе сравнение пустое)"), Case.Name), Batched->Num() > 0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FColorRampTest,
	"CellularAutomata.Rendering.ColorRamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FColorRampTest::RunTest(const FString& Parameters)
{
	const TArray<EColorRampSpace> Spaces = {
		EColorRampSpace::LinearRgb, EColorRampSpace::Srgb,
		EColorRampSpace::Oklab, EColorRampSpace::Oklch
	};
	const TArray<EColorRampCurve> Curves = { EColorRampCurve::Linear, EColorRampCurve::CatmullRom };

	auto SpaceName = [](EColorRampSpace Space)
	{
		switch (Space)
		{
		case EColorRampSpace::Srgb:  return TEXT("Srgb");
		case EColorRampSpace::Oklab: return TEXT("Oklab");
		case EColorRampSpace::Oklch: return TEXT("Oklch");
		default:                     return TEXT("LinearRgb");
		}
	};
	auto CurveName = [](EColorRampCurve Curve)
	{
		return (Curve == EColorRampCurve::CatmullRom) ? TEXT("CatmullRom") : TEXT("Linear");
	};

	// Намеренно разношёрстный набор: насыщенные основные, тёмный, светлый и
	// СЕРЫЙ. Серый здесь не для полноты - у него не определён тон, и именно на
	// нём ломается наивная реализация Oklch (atan2 от нуля даёт произвольный
	// угол, и рампа дёргается в случайную сторону).
	const TArray<FLinearColor> Keys = {
		FLinearColor(1.0f, 0.0f, 0.0f),
		FLinearColor(0.5f, 0.5f, 0.5f),
		FLinearColor(0.0f, 0.35f, 1.0f),
		FLinearColor(1.0f, 0.9f, 0.1f)
	};

	for (EColorRampSpace Space : Spaces)
	{
		for (EColorRampCurve Curve : Curves)
		{
			const FString Case = FString::Printf(TEXT("%s/%s"), SpaceName(Space), CurveName(Curve));

			// ГЛАВНАЯ ГАРАНТИЯ: в опорных точках обязан получаться сам ключ.
			// Это то единственное, что ловит ошибку туда-обратного
			// преобразования - она иначе проявляется как еле заметный сдвиг
			// оттенка по ВСЕЙ рампе, который глазом не отличить от задуманного
			// цвета. Допуск 1/255: ниже него разница всё равно теряется при
			// квантовании в FColor (см. FCellRenderInstance).
			constexpr float Tolerance = 1.0f / 255.0f;
			for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
			{
				const float T = float(KeyIndex) / float(Keys.Num() - 1);
				const FLinearColor Got = ColorRamp::Sample(Keys, T, Space, Curve);
				const FLinearColor& Want = Keys[KeyIndex];

				if (FMath::Abs(Got.R - Want.R) > Tolerance
					|| FMath::Abs(Got.G - Want.G) > Tolerance
					|| FMath::Abs(Got.B - Want.B) > Tolerance)
				{
					AddError(FString::Printf(
						TEXT("%s: в опорной точке %d (T=%.3f) получено (%.4f, %.4f, %.4f) вместо (%.4f, %.4f, %.4f)"),
						*Case, KeyIndex, T, Got.R, Got.G, Got.B, Want.R, Want.G, Want.B));
				}
			}

			// Ничего за пределами гаммы: Катмулл-Ром вылетает за диапазон
			// опорных значений по построению, и без зажима это дало бы
			// отрицательные компоненты, которые дальше молча превратились бы в
			// мусор при квантовании.
			bool bOutOfRange = false;
			for (int32 Step = 0; Step <= 256 && !bOutOfRange; ++Step)
			{
				const FLinearColor Got = ColorRamp::Sample(Keys, float(Step) / 256.0f, Space, Curve);
				// Проверка на конечность идёт ПЕРВОЙ и отдельно: сравнения с
				// NaN всегда ложны, поэтому диапазонные условия ниже пропустили
				// бы его молча.
				if (!FMath::IsFinite(Got.R) || !FMath::IsFinite(Got.G) || !FMath::IsFinite(Got.B)
					|| Got.R < 0.0f || Got.R > 1.0f || Got.G < 0.0f || Got.G > 1.0f
					|| Got.B < 0.0f || Got.B > 1.0f)
				{
					AddError(FString::Printf(TEXT("%s: цвет вне [0,1] или NaN при T=%.4f - (%.4f, %.4f, %.4f)"),
						*Case, float(Step) / 256.0f, Got.R, Got.G, Got.B));
					bOutOfRange = true;
				}
			}
		}
	}

	// Вырожденные входы - ровно то поведение, на которое опирается панель
	// настройки: пустая рампа не ошибка, а "материал как есть".
	const TArray<FLinearColor> Empty;
	const FLinearColor FromEmpty = ColorRamp::Sample(Empty, 0.5f, EColorRampSpace::Oklab, EColorRampCurve::CatmullRom);
	if (!FromEmpty.Equals(FLinearColor::White, KINDA_SMALL_NUMBER))
	{
		AddError(TEXT("пустая рампа обязана давать белый"));
	}

	const TArray<FLinearColor> Single = { FLinearColor(0.2f, 0.7f, 0.3f) };
	for (float T : { 0.0f, 0.5f, 1.0f })
	{
		const FLinearColor Got = ColorRamp::Sample(Single, T, EColorRampSpace::Oklch, EColorRampCurve::CatmullRom);
		if (!Got.Equals(Single[0], KINDA_SMALL_NUMBER))
		{
			AddError(FString::Printf(TEXT("рампа из одного ключа обязана давать его же при любом T (T=%.1f)"), T));
		}
	}

	// Смена пространства обязана что-то МЕНЯТЬ в середине - иначе тест выше
	// проходил бы и на реализации, которая молча игнорирует настройку и всегда
	// смешивает в линейном RGB.
	{
		const TArray<FLinearColor> Pair = { FLinearColor(1.0f, 0.0f, 0.0f), FLinearColor(0.0f, 0.0f, 1.0f) };
		const FLinearColor Lin = ColorRamp::Sample(Pair, 0.5f, EColorRampSpace::LinearRgb, EColorRampCurve::Linear);
		const FLinearColor Lab = ColorRamp::Sample(Pair, 0.5f, EColorRampSpace::Oklab, EColorRampCurve::Linear);
		if (Lin.Equals(Lab, 1.0f / 255.0f))
		{
			AddError(TEXT("середина рампы в LinearRgb и Oklab совпала - похоже, пространство не учитывается"));
		}
		else
		{
			AddInfo(FString::Printf(TEXT("середина красный->синий: LinearRgb (%.3f, %.3f, %.3f), Oklab (%.3f, %.3f, %.3f)"),
				Lin.R, Lin.G, Lin.B, Lab.R, Lab.G, Lab.B));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCellParityFilterTest,
	"CellularAutomata.Generation.ParityFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FCellParityFilterTest::RunTest(const FString& Parameters)
{
	// Отбор по чётности (FStateGeneratorParams::ParityFilter) - это весь
	// ГЦК-режим целиком, поэтому проверяется не "фильтр что-то отфильтровал", а
	// два утверждения, на которых он держится.
	//
	// ПЕРВОЕ: фильтр РАЗБИВАЕТ набор, а не режет его. Even и Odd, слитые
	// обратно, обязаны дать в точности то же, что даёт None - иначе фильтр
	// теряет клетки помимо чётности. Именно это ловит главную ловушку в
	// FCellEmitter::Emit(): отказ по чётности обязан возвращать true, а false
	// там означает переполнение и обрывает генератор целиком, так что при
	// ошибке набор оказался бы обрезан по первому же узлу не той чётности.
	// Проверка идёт по КАЖДОМУ типу генератора, потому что мимо воронки Emit()
	// мог бы пройти отдельный генератор.
	{
		const int32 TypeCount = static_cast<int32>(EStateGeneratorType::SymmetricSeed) + 1;

		for (int32 TypeIndex = 0; TypeIndex < TypeCount; ++TypeIndex)
		{
			const EStateGeneratorType Type = static_cast<EStateGeneratorType>(TypeIndex);
			const FString Name = StateGenerators::GetDisplayName(Type);

			FStateGeneratorParams Params;
			Params.Type = Type;
			Params.Extent = FIntVector(10, 10, 10);
			Params.Radius = 7;
			Params.Amount = 200;
			Params.ClusterCount = 5;
			Params.ClusterRadius = 3;
			Params.CoreExtent = FIntVector(3, 3, 3);

			TArray<FIntVector> All;
			TArray<FIntVector> Even;
			TArray<FIntVector> Odd;
			StateGenerators::FGenerateStats Stats;
			FString Error;

			Params.ParityFilter = ECellParityFilter::None;
			if (!StateGenerators::Generate(Params, /*Seed=*/4242, MAX_int64, All, Stats, Error))
			{
				AddError(FString::Printf(TEXT("%s: генерация без фильтра не удалась - %s"), *Name, *Error));
				continue;
			}

			Params.ParityFilter = ECellParityFilter::Even;
			if (!StateGenerators::Generate(Params, /*Seed=*/4242, MAX_int64, Even, Stats, Error))
			{
				AddError(FString::Printf(TEXT("%s: генерация с чётным фильтром не удалась - %s"), *Name, *Error));
				continue;
			}

			Params.ParityFilter = ECellParityFilter::Odd;
			if (!StateGenerators::Generate(Params, /*Seed=*/4242, MAX_int64, Odd, Stats, Error))
			{
				AddError(FString::Printf(TEXT("%s: генерация с нечётным фильтром не удалась - %s"), *Name, *Error));
				continue;
			}

			bool bParityHolds = true;
			for (const FIntVector& Cell : Even)
			{
				if (((Cell.X + Cell.Y + Cell.Z) & 1) != 0)
				{
					AddError(FString::Printf(TEXT("%s: при фильтре Even попалась клетка нечётной суммы (%d,%d,%d)"),
						*Name, Cell.X, Cell.Y, Cell.Z));
					bParityHolds = false;
					break;
				}
			}

			for (const FIntVector& Cell : Odd)
			{
				if (((Cell.X + Cell.Y + Cell.Z) & 1) == 0)
				{
					AddError(FString::Printf(TEXT("%s: при фильтре Odd попалась клетка чётной суммы (%d,%d,%d)"),
						*Name, Cell.X, Cell.Y, Cell.Z));
					bParityHolds = false;
					break;
				}
			}

			if (!bParityHolds)
			{
				continue;
			}

			// Сравнение МНОЖЕСТВАМИ, а не длинами массивов, и это не
			// педантизм: часть генераторов выдаёт одну и ту же клетку по
			// нескольку раз (RandomBall - reject-sampling с коллизиями, см.
			// NeedsDedupe()), так что "Even.Num() + Odd.Num()" считает повторы
			// и к размеру объединения отношения не имеет. Утверждение здесь
			// именно про состав набора, а не про его длину.
			const TSet<FIntVector> EvenSet(Even);
			const TSet<FIntVector> OddSet(Odd);
			const TSet<FIntVector> Reference(All);

			if (EvenSet.Intersect(OddSet).Num() != 0)
			{
				AddError(FString::Printf(TEXT("%s: Even и Odd пересекаются - одна клетка не может быть обеих чётностей"),
					*Name));
				continue;
			}

			TSet<FIntVector> Union(EvenSet);
			Union.Append(OddSet);

			if (Union.Num() != Reference.Num() || Union.Difference(Reference).Num() != 0)
			{
				AddError(FString::Printf(
					TEXT("%s: Even+Odd не равно набору без фильтра (%d против %d) - фильтр теряет клетки помимо чётности"),
					*Name, Union.Num(), Reference.Num()));
				continue;
			}

			AddInfo(FString::Printf(TEXT("%s: %d уникальных клеток = %d чётных + %d нечётных"),
				*Name, Reference.Num(), EvenSet.Num(), OddSet.Num()));
		}
	}

	// ВТОРОЕ: замкнутость ГЦК-подрешётки. Соседство Edges - это ровно 12
	// смещений с d^2 == 2, каждое меняет сумму координат на 0 или +-2, поэтому
	// автомат, засеянный чётными клетками, обязан остаться чётным НАВСЕГДА.
	// Это и есть всё утверждение "ГЦК уже работает без новой геометрии": если
	// оно ложно, режим не решётка, а просто прореженный куб.
	{
		constexpr float CellSize = 100.0f;
		constexpr int32 ChunkSize = 16;

		FStateGeneratorParams Params;
		Params.Type = EStateGeneratorType::SolidSphere;
		Params.Radius = 9;
		Params.ParityFilter = ECellParityFilter::Even;

		TArray<FIntVector> SeedCells;
		StateGenerators::FGenerateStats Stats;
		FString Error;

		if (!StateGenerators::Generate(Params, /*Seed=*/7, MAX_int64, SeedCells, Stats, Error))
		{
			AddError(FString::Printf(TEXT("не удалось построить ГЦК-затравку - %s"), *Error));
			return true;
		}

		// Правило подобрано так, чтобы шаг заведомо что-то РОДИЛ: проверка "все
		// живые клетки чётные" на пустой сетке прошла бы пустым множеством.
		const TArray<int32> Birth = { 1, 2, 3 };
		const TArray<int32> Survival = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

		struct FParityCase
		{
			ENeighborhood Neighborhood;
			bool bExpectParityPreserved;
			const TCHAR* Name;
		};

		// Moore здесь не для полноты, а как встречная проверка: он чётность
		// заведомо ломает (грани меняют сумму на +-1), и если тест проходит и
		// для него тоже - значит проверка вырожденная и ничего не измеряет.
		const FParityCase Cases[] = {
			{ ENeighborhood::Edges,        true,  TEXT("Edges") },
			{ ENeighborhood::EdgesFarAxes, true,  TEXT("EdgesFarAxes") },
			{ ENeighborhood::Moore,        false, TEXT("Moore") },
		};

		for (const FParityCase& Case : Cases)
		{
			const FCellularAutomatonRule Rule(Birth, Survival, Case.Neighborhood, /*States=*/2);
			const FCpuComputeStrategy Strategy;

			FDenseCellGrid Current(CellSize, ChunkSize, /*bEnableDecay=*/false);
			for (const FIntVector& Cell : SeedCells)
			{
				Current.SetAlive(Cell, true);
			}

			FDenseCellGrid Next(CellSize, ChunkSize, /*bEnableDecay=*/false);
			Strategy.Step(Current, Next, Rule);

			TArray<FIntVector> Alive;
			Next.GetAliveCells(Alive);

			if (Alive.Num() == 0)
			{
				AddError(FString::Printf(TEXT("%s: после шага не осталось ни одной клетки - проверка была бы пустой"),
					Case.Name));
				continue;
			}

			int32 OddCount = 0;
			for (const FIntVector& Cell : Alive)
			{
				if (((Cell.X + Cell.Y + Cell.Z) & 1) != 0)
				{
					++OddCount;
				}
			}

			if (Case.bExpectParityPreserved)
			{
				if (OddCount != 0)
				{
					AddError(FString::Printf(
						TEXT("%s: чётность не сохранилась - %d из %d клеток ушли на другую подрешётку"),
						Case.Name, OddCount, Alive.Num()));
					continue;
				}

				AddInfo(FString::Printf(TEXT("%s: %d клеток, все на ГЦК-подрешётке"), Case.Name, Alive.Num()));
			}
			else
			{
				if (OddCount == 0)
				{
					AddError(FString::Printf(
						TEXT("%s: чётность неожиданно сохранилась - встречная проверка выродилась, ")
						TEXT("основная перестала что-либо доказывать"),
						Case.Name));
					continue;
				}

				AddInfo(FString::Printf(TEXT("%s: %d из %d клеток нечётные, как и ожидалось"),
					Case.Name, OddCount, Alive.Num()));
			}
		}
	}

	// ТРЕТЬЕ: то же самое для ОЦК. Условие здесь ДРУГОЕ - не чётность суммы, а
	// одинаковая чётность всех трёх координат (см. ECellParityFilter::SameParity),
	// поэтому и проверка своя, а не переиспользованная сверху.
	{
		constexpr float CellSize = 100.0f;
		constexpr int32 ChunkSize = 16;

		auto IsSameParity = [](const FIntVector& Cell)
		{
			const int32 ParityX = Cell.X & 1;
			return (Cell.Y & 1) == ParityX && (Cell.Z & 1) == ParityX;
		};

		FStateGeneratorParams Params;
		Params.Type = EStateGeneratorType::SolidSphere;
		Params.Radius = 9;
		Params.ParityFilter = ECellParityFilter::SameParity;

		TArray<FIntVector> SeedCells;
		StateGenerators::FGenerateStats Stats;
		FString Error;

		if (!StateGenerators::Generate(Params, /*Seed=*/11, MAX_int64, SeedCells, Stats, Error))
		{
			AddError(FString::Printf(TEXT("не удалось построить ОЦК-затравку - %s"), *Error));
			return true;
		}

		for (const FIntVector& Cell : SeedCells)
		{
			if (!IsSameParity(Cell))
			{
				AddError(FString::Printf(TEXT("ОЦК-затравка: клетка (%d,%d,%d) не одной чётности"),
					Cell.X, Cell.Y, Cell.Z));
				break;
			}
		}

		const TArray<int32> Birth = { 1, 2, 3 };
		const TArray<int32> Survival = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };

		struct FBccCase
		{
			ENeighborhood Neighborhood;
			bool bExpectPreserved;
			const TCHAR* Name;
		};

		// Corners - восемь диагоналей куба, ближайшие соседи ОЦК; вместе с
		// FarAxes (шесть осевых на расстоянии 2) получается четырнадцать, то
		// есть ровно число граней усечённого октаэдра. Moore снова встречная
		// проверка: он ломает условие заведомо, и если тест проходит и для него,
		// значит он вырожденный.
		const FBccCase Cases[] = {
			{ ENeighborhood::Corners,        true,  TEXT("Corners") },
			{ ENeighborhood::CornersFarAxes, true,  TEXT("CornersFarAxes") },
			{ ENeighborhood::Moore,          false, TEXT("Moore") },
		};

		for (const FBccCase& Case : Cases)
		{
			const FCellularAutomatonRule Rule(Birth, Survival, Case.Neighborhood, /*States=*/2);
			const FCpuComputeStrategy Strategy;

			FDenseCellGrid Current(CellSize, ChunkSize, /*bEnableDecay=*/false);
			for (const FIntVector& Cell : SeedCells)
			{
				Current.SetAlive(Cell, true);
			}

			FDenseCellGrid Next(CellSize, ChunkSize, /*bEnableDecay=*/false);
			Strategy.Step(Current, Next, Rule);

			TArray<FIntVector> Alive;
			Next.GetAliveCells(Alive);

			if (Alive.Num() == 0)
			{
				AddError(FString::Printf(TEXT("ОЦК %s: после шага пусто - проверка была бы вырожденной"), Case.Name));
				continue;
			}

			int32 StrayCount = 0;
			for (const FIntVector& Cell : Alive)
			{
				if (!IsSameParity(Cell))
				{
					++StrayCount;
				}
			}

			if (Case.bExpectPreserved && StrayCount != 0)
			{
				AddError(FString::Printf(
					TEXT("ОЦК %s: подрешётка не замкнута - %d из %d клеток ушли с неё"),
					Case.Name, StrayCount, Alive.Num()));
			}
			else if (!Case.bExpectPreserved && StrayCount == 0)
			{
				AddError(FString::Printf(
					TEXT("ОЦК %s: условие неожиданно сохранилось - встречная проверка выродилась"), Case.Name));
			}
			else
			{
				AddInfo(FString::Printf(TEXT("ОЦК %s: %d клеток, ушедших с подрешётки %d"),
					Case.Name, Alive.Num(), StrayCount));
			}
		}
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
	Params.CellWorldStep = FVector(CellSize);
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
	Params.CellWorldStep = FVector(CellSize);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerationHistoryTest,
	"CellularAutomata.GenerationHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGenerationHistoryTest::RunTest(const FString& Parameters)
{
	// Засев после ResetGenerationCounter(): история пуста, а рендер уже
	// случился - замер обязан появиться, иначе поколение 0 (самое интересное:
	// исходный паттерн) не попадёт на график вовсе.
	{
		TArray<FGenerationSample> History;
		GenerationHistory::NoteRendered(History, 0, 1000, 800, 8);
		TestEqual(TEXT("рендер на пустой истории засевает замер"), History.Num(), 1);
		TestEqual(TEXT("живые"), History.Last().AliveCount, 1000);
		TestEqual(TEXT("видимые"), History.Last().RenderedCount, 800);
	}

	// Главная защита всей схемы: RefreshRenderCullVolume() дёргает рендер из
	// Tick() на каждое движение камеры при включённом срезе, поколение при этом
	// не меняется. Без правки последнего замера на месте один полёт камеры
	// размазал бы одно поколение на сотни точек и выдавил бы из окна всё
	// остальное.
	{
		TArray<FGenerationSample> History;
		GenerationHistory::NoteRendered(History, 7, 1000, 500, 512);
		for (int32 Index = 0; Index < 1000; ++Index)
		{
			GenerationHistory::NoteRendered(History, 7, 1000 + Index, 600 + Index, 512);
		}
		TestEqual(TEXT("рендер без смены поколения не плодит замеры"), History.Num(), 1);
		TestEqual(TEXT("замер поправлен на месте"), History.Last().RenderedCount, 1599);
		// DeleteSelectedCells() правит сетку прямо на паузе - число живых
		// обязано поехать в той же точке, без нового поколения.
		TestEqual(TEXT("живые тоже обновились"), History.Last().AliveCount, 1999);

		GenerationHistory::NoteRendered(History, 8, 2000, 700, 512);
		TestEqual(TEXT("новое поколение добавляет замер"), History.Num(), 2);
	}

	// Перенос "видимо" вперёд: при StepsPerRender > 1 большинство поколений на
	// экран не попадают, и линия обязана держать последнее известное значение
	// ступенькой, а не падать в ноль.
	{
		TArray<FGenerationSample> History;
		GenerationHistory::Append(History, 0, 100, 512);
		TestEqual(TEXT("первый замер без истории даёт 0"), History.Last().RenderedCount, 0);

		GenerationHistory::NoteRendered(History, 0, 100, 90, 512);
		GenerationHistory::Append(History, 1, 120, 512);
		TestEqual(TEXT("поколение без рендера наследует прошлое значение"), History.Last().RenderedCount, 90);

		GenerationHistory::Append(History, 2, 140, 512);
		TestEqual(TEXT("перенос идёт дальше по цепочке"), History.Last().RenderedCount, 90);
	}

	// Срез по ёмкости: окно скользит, память не растёт.
	{
		TArray<FGenerationSample> History;
		for (int64 Generation = 0; Generation <= 10; ++Generation)
		{
			GenerationHistory::Append(History, Generation, 10, 4);
		}
		TestEqual(TEXT("ёмкость соблюдена"), History.Num(), 4);
		TestEqual(TEXT("выброшено самое старое"), History[0].Generation, (int64)7);
		TestEqual(TEXT("самое свежее на месте"), History.Last().Generation, (int64)10);

		// GenerationHistoryCapacity правится в Details panel на живом акторе -
		// прийти сюда с уменьшенным значением можно, отстав на сотни замеров.
		GenerationHistory::Append(History, 11, 10, 2);
		TestEqual(TEXT("уменьшение ёмкости подрезает сразу"), History.Num(), 2);
	}

	// Обрезка хвоста под шаг назад (Ctrl+Z, StepBackward()): точка отката
	// остаётся, всё, что после неё - уходит, а история ДО отката переживает его.
	// В этом вся разница с ResetGenerationCounter(), который чистит график
	// целиком: шаг назад не начинает прогон заново.
	{
		TArray<FGenerationSample> History;
		for (int64 Generation = 0; Generation <= 10; ++Generation)
		{
			GenerationHistory::Append(History, Generation, 100 + int32(Generation), 512);
		}

		GenerationHistory::TrimAfter(History, 7);
		TestEqual(TEXT("хвост после точки отката срезан"), History.Num(), 8);
		TestEqual(TEXT("сама точка отката осталась"), History.Last().Generation, (int64)7);
		TestEqual(TEXT("история до отката не тронута"), History[0].Generation, (int64)0);

		// Откат в самое начало оставляет ровно поколение 0 - не пустой график:
		// StepBackward() на поколении 1 делегирует в ResetToInitialState(), и
		// нулевая точка обязана пережить это, иначе линия начнётся с пустоты.
		GenerationHistory::TrimAfter(History, 0);
		TestEqual(TEXT("остаётся один нулевой замер"), History.Num(), 1);
		TestEqual(TEXT("это поколение 0"), History.Last().Generation, (int64)0);

		// Поколение выше всех имеющихся - не трогает ничего (шаг назад сразу
		// после загрузки файла, когда история ещё короче счётчика).
		GenerationHistory::TrimAfter(History, 1000);
		TestEqual(TEXT("обрезка выше окна ничего не меняет"), History.Num(), 1);
	}

	// Границы окна. При States > 2 угасающие клетки рисуются, но живыми не
	// считаются - "видимо" ЗАКОННО выше "всего", и масштаб по одному ряду
	// срезал бы второй.
	{
		TArray<FGenerationSample> History;
		int64 MinGeneration = -1;
		int64 MaxGeneration = -1;
		double MinY = -1.0;
		double MaxY = -1.0;

		TestFalse(TEXT("на пустой истории границ нет"),
			GenerationHistory::ComputeBounds(History, 0.0, MinGeneration, MaxGeneration, MinY, MaxY));

		GenerationHistory::NoteRendered(History, 5, 100, 900, 512);
		TestTrue(TEXT("границы одного замера"),
			GenerationHistory::ComputeBounds(History, 0.0, MinGeneration, MaxGeneration, MinY, MaxY));
		TestEqual(TEXT("вырожденное окно"), MinGeneration, MaxGeneration);
		TestEqual(TEXT("потолок берётся по обоим рядам"), MaxY, 900.0);
		TestEqual(TEXT("дно берётся по обоим рядам"), MinY, 100.0);

		GenerationHistory::Append(History, 9, 2000, 512);
		TestTrue(TEXT("границы двух замеров"),
			GenerationHistory::ComputeBounds(History, 0.0, MinGeneration, MaxGeneration, MinY, MaxY));
		TestEqual(TEXT("левый край"), MinGeneration, (int64)5);
		TestEqual(TEXT("правый край"), MaxGeneration, (int64)9);
		TestEqual(TEXT("потолок поднялся до живых"), MaxY, 2000.0);
	}

	// Нормировка: поколение 0 делить не на что, и такой замер выпадает целиком -
	// вместе с левым краем окна, который до этого был просто концом массива.
	{
		TArray<FGenerationSample> History;
		GenerationHistory::NoteRendered(History, 0, 1000, 1000, 512);
		GenerationHistory::NoteRendered(History, 2, 80, 80, 512);
		GenerationHistory::NoteRendered(History, 4, 640, 640, 512);

		int64 MinGeneration = -1;
		int64 MaxGeneration = -1;
		double MinY = -1.0;
		double MaxY = -1.0;

		TestTrue(TEXT("границы при нормировке"),
			GenerationHistory::ComputeBounds(History, 3.0, MinGeneration, MaxGeneration, MinY, MaxY));
		// Поколение 0 выброшено - иначе левый край остался бы нулём, а деление
		// на него ушло бы в бесконечность и утащило бы в NaN всю ломаную.
		TestEqual(TEXT("левый край сдвинулся"), MinGeneration, (int64)2);
		TestEqual(TEXT("правый край на месте"), MaxGeneration, (int64)4);
		// 80/8 = 10, 640/64 = 10 - ровное плато, ради которого нормировка и
		// затевалась: население выросло в восемь раз, отношение не шелохнулось.
		TestTrue(TEXT("плато по нормированному значению"), FMath::IsNearlyEqual(MinY, 10.0, 1e-9));
		TestTrue(TEXT("плато не разъехалось"), FMath::IsNearlyEqual(MaxY, 10.0, 1e-9));

		// История из одного лишь нулевого поколения при нормировке нерисуема.
		TArray<FGenerationSample> ZeroOnly;
		GenerationHistory::NoteRendered(ZeroOnly, 0, 1000, 1000, 512);
		TestFalse(TEXT("одно нулевое поколение - рисовать нечего"),
			GenerationHistory::ComputeBounds(ZeroOnly, 3.0, MinGeneration, MaxGeneration, MinY, MaxY));
		TestTrue(TEXT("а без нормировки оно рисуемо"),
			GenerationHistory::ComputeBounds(ZeroOnly, 0.0, MinGeneration, MaxGeneration, MinY, MaxY));
	}

	// Само нормированное значение и подгонка показателя - то, ради чего вся
	// затея: по чистой степени наклон обязан выйти точно.
	{
		TestEqual(TEXT("без нормировки значение не трогают"),
			GenerationHistory::NormalizedValue(4, 640, 0.0), 640.0);
		TestTrue(TEXT("деление на n^d"),
			FMath::IsNearlyEqual(GenerationHistory::NormalizedValue(4, 640, 3.0), 10.0, 1e-9));
		// Ноль вместо бесконечности: одна inf в массиве точек уводит в NaN всю
		// ломаную, и график пропадает целиком.
		TestEqual(TEXT("поколение 0 не даёт бесконечность"),
			GenerationHistory::NormalizedValue(0, 640, 3.0), 0.0);

		TArray<FGenerationSample> History;
		for (int64 Generation = 1; Generation <= 16; ++Generation)
		{
			// Ровно n^3 - подгонка обязана вернуть тройку.
			GenerationHistory::Append(History, Generation,
				int32(Generation * Generation * Generation), 512);
		}
		TestTrue(TEXT("показатель куба измерен"),
			FMath::IsNearlyEqual(GenerationHistory::FitGrowthExponent(History), 3.0, 1e-6));

		// Вымершая сетка (log 0) и поколение 0 (log 0 по X) не участвуют, но и
		// не роняют подгонку в NaN.
		GenerationHistory::Append(History, 17, 0, 512);
		TestTrue(TEXT("нули не портят подгонку"),
			FMath::IsNearlyEqual(GenerationHistory::FitGrowthExponent(History), 3.0, 1e-6));

		TArray<FGenerationSample> TooShort;
		GenerationHistory::Append(TooShort, 5, 100, 512);
		TestEqual(TEXT("по одной точке наклона нет"),
			GenerationHistory::FitGrowthExponent(TooShort), 0.0);

		// Все замеры на одном поколении - окно из одной вертикали.
		TArray<FGenerationSample> OneGeneration;
		GenerationHistory::Append(OneGeneration, 5, 100, 512);
		GenerationHistory::Append(OneGeneration, 5, 200, 512);
		TestEqual(TEXT("вертикаль не даёт наклона"),
			GenerationHistory::FitGrowthExponent(OneGeneration), 0.0);
	}

	// Отрезок оси для нормированного графика: узкую полосу нельзя мерить от
	// нуля - от него она выглядит прямой линией.
	{
		double Min = -1.0;
		double Max = -1.0;

		GenerationHistory::ComputeNiceRange(0.9026, 1.3333, Min, Max);
		TestTrue(TEXT("дно поднято над нулём"), Min > 0.0);
		TestTrue(TEXT("дно не выше данных"), Min <= 0.9026);
		TestTrue(TEXT("потолок не ниже данных"), Max >= 1.3333);
		// Полоса шириной 0.43 внутри отрезка шириной не больше 1 - иначе от
		// колебаний, ради которых всё и делается, остаётся плоская линия.
		TestTrue(TEXT("отрезок не растянут"), (Max - Min) <= 1.0);

		// Вырожденные данные не должны давать нулевой ширины: на неё делят.
		GenerationHistory::ComputeNiceRange(5.0, 5.0, Min, Max);
		TestTrue(TEXT("плато не даёт нулевого отрезка"), Max > Min);

		GenerationHistory::ComputeNiceRange(0.0, 0.0, Min, Max);
		TestTrue(TEXT("нули не дают нулевого отрезка"), Max > Min);
	}

	// Раскладка по X идёт по ЗНАЧЕНИЮ поколения, а не по индексу: один заход
	// GPU считает пачку переменного размера, замеры в окне стоят неравномерно,
	// и раскладка по индексу врала бы о том, где что произошло.
	{
		TArray<FGenerationSample> History;
		GenerationHistory::NoteRendered(History, 0, 50, 50, 512);
		GenerationHistory::NoteRendered(History, 10, 100, 100, 512);  // шаг 10
		GenerationHistory::NoteRendered(History, 11, 100, 100, 512);  // шаг 1

		TArray<FVector2f> Alive, Rendered;
		GenerationHistory::MapToPoints(History, FVector2f(110.0f, 100.0f), FVector2f::ZeroVector,
			0, 11, /*MinY=*/0.0, /*MaxY=*/100.0, /*bLogScale=*/false, /*Exponent=*/0.0,
			Alive, Rendered);

		TestEqual(TEXT("точек столько же, сколько замеров"), Alive.Num(), 3);
		TestEqual(TEXT("левый край"), Alive[0].X, 0.0f);
		// 10/11 от ширины, а не 1/2: индекс тут дал бы 55.
		TestTrue(TEXT("X пропорционален поколению, а не индексу"), FMath::IsNearlyEqual(Alive[1].X, 100.0f, 0.01f));
		TestTrue(TEXT("правый край"), FMath::IsNearlyEqual(Alive[2].X, 110.0f, 0.01f));
		// Y инвертирован: значение вверх, координата вниз.
		TestTrue(TEXT("полное значение - верх области"), FMath::IsNearlyEqual(Alive[1].Y, 0.0f, 0.01f));
		TestTrue(TEXT("половина значения - середина"), FMath::IsNearlyEqual(Alive[0].Y, 50.0f, 0.01f));
	}

	// Вырожденное окно (все замеры на одном поколении) не должно делить на ноль.
	{
		TArray<FGenerationSample> History;
		GenerationHistory::Append(History, 3, 10, 512);
		GenerationHistory::Append(History, 3, 20, 512);

		TArray<FVector2f> Alive, Rendered;
		GenerationHistory::MapToPoints(History, FVector2f(100.0f, 100.0f), FVector2f::ZeroVector,
			3, 3, /*MinY=*/0.0, /*MaxY=*/20.0, /*bLogScale=*/false, /*Exponent=*/0.0,
			Alive, Rendered);

		TestEqual(TEXT("точки построены"), Alive.Num(), 2);
		TestTrue(TEXT("X конечен"), FMath::IsFinite(Alive[0].X) && FMath::IsFinite(Alive[1].X));
		TestTrue(TEXT("Y конечен"), FMath::IsFinite(Alive[0].Y) && FMath::IsFinite(Alive[1].Y));
	}

	// Поднятое дно и нормировка в самой раскладке: замер на поколении 0
	// выбрасывается из ОБОИХ рядов сразу, иначе они разъедутся по длине и
	// вторая ломаная поедет по чужим X.
	{
		TArray<FGenerationSample> History;
		GenerationHistory::NoteRendered(History, 0, 1000, 1000, 512);
		GenerationHistory::NoteRendered(History, 2, 80, 40, 512);
		GenerationHistory::NoteRendered(History, 4, 640, 320, 512);

		TArray<FVector2f> Alive, Rendered;
		// Значения после нормировки: живые - ровно 10, видимые - ровно 5.
		// Отрезок [5, 10] кладёт первую линию на верх области, вторую - на низ.
		GenerationHistory::MapToPoints(History, FVector2f(100.0f, 100.0f), FVector2f::ZeroVector,
			2, 4, /*MinY=*/5.0, /*MaxY=*/10.0, /*bLogScale=*/false, /*Exponent=*/3.0,
			Alive, Rendered);

		TestEqual(TEXT("нулевое поколение выброшено"), Alive.Num(), 2);
		TestEqual(TEXT("ряды одной длины"), Rendered.Num(), Alive.Num());
		TestTrue(TEXT("левый край - первое пригодное поколение"),
			FMath::IsNearlyEqual(Alive[0].X, 0.0f, 0.01f));
		TestTrue(TEXT("значение на потолке - верх области"),
			FMath::IsNearlyEqual(Alive[0].Y, 0.0f, 0.01f));
		// Дно отрезка, а не ноль: на обычном графике эта линия ушла бы в
		// середину области и полоса значений выглядела бы вдвое шире.
		TestTrue(TEXT("значение на дне - низ области"),
			FMath::IsNearlyEqual(Rendered[0].Y, 100.0f, 0.01f));
	}

	// "Красивый" потолок: 1/2/5 * 10^k, и ноль не роняет масштаб в ноль.
	{
		TestEqual(TEXT("ноль даёт единицу"), GenerationHistory::NiceCeiling(0.0), 1.0);
		TestEqual(TEXT("1234 -> 2000"), GenerationHistory::NiceCeiling(1234.0), 2000.0);
		TestEqual(TEXT("6000 -> 10000"), GenerationHistory::NiceCeiling(6000.0), 10000.0);
		TestEqual(TEXT("ровное значение не поднимается"), GenerationHistory::NiceCeiling(2000.0), 2000.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLatticeOrthogonalRoundTripTest,
	"CellularAutomata.Lattice.OrthogonalRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FLatticeOrthogonalRoundTripTest::RunTest(const FString& Parameters)
{
	// Обратного преобразования в проекте раньше не существовало вовсе - вместо
	// него в четырёх местах стояло написанное от руки деление на CellSize. Тест
	// закрывает именно ту дыру: WorldToGrid() обязан быть точной обратной к
	// GridToWorld() при ЛЮБОМ шаге по осям, включая неравный.
	const TArray<float> CellSizes = { 1.0f, 100.0f, 37.5f };
	const TArray<float> ZScales = { 1.0f, 2.0f, 0.5f };

	for (float CellSize : CellSizes)
	{
		for (float ZScale : ZScales)
		{
			const FLatticeTransform Lattice = FLatticeTransform::MakeOrthogonal(CellSize, ZScale);

			// Диапазон захватывает отрицательные координаты: генерация
			// центрирована в нуле, так что они норма, а не крайний случай (та
			// же причина, по которой существует Grid.NegativeCoords).
			for (int32 X = -40; X <= 40; X += 7)
			{
				for (int32 Y = -40; Y <= 40; Y += 11)
				{
					for (int32 Z = -40; Z <= 40; Z += 13)
					{
						const FIntVector Cell(X, Y, Z);
						const FIntVector RoundTripped = Lattice.WorldToGrid(Lattice.GridToWorld(Cell));
						if (RoundTripped != Cell)
						{
							AddError(FString::Printf(TEXT("WorldToGrid(GridToWorld(%s)) вернул %s при CellSize=%.1f, ZScale=%.1f"),
								*Cell.ToString(), *RoundTripped.ToString(), CellSize, ZScale));
							return false;
						}
					}
				}
			}

			// Точка ВНУТРИ клетки, а не её центр: клетка занимает полшага в
			// каждую сторону, и попадание любой внутренней точки в свою клетку -
			// то, на чём стоит DDA-пик.
			const FIntVector Probe(3, -5, 7);
			const FVector Inside = Lattice.GridToWorld(Probe) + Lattice.GetCellWorldExtent() * 0.49;
			if (Lattice.WorldToGrid(Inside) != Probe)
			{
				AddError(FString::Printf(TEXT("Точка внутри клетки %s отнесена к %s (CellSize=%.1f, ZScale=%.1f)"),
					*Probe.ToString(), *Lattice.WorldToGrid(Inside).ToString(), CellSize, ZScale));
				return false;
			}
		}
	}

	{
		// Рамка обязана НАКРЫВАТЬ каждую клетку, чей центр внутри бокса
		// (проверка надмножеством: она сознательно консервативна, чуть шире
		// точной, потому что вызывающие после неё проверяют каждую клетку сами,
		// а вот потерянная клетка была бы дыркой на границе куба отсечения).
		const FLatticeTransform Lattice = FLatticeTransform::MakeOrthogonal(100.0f, 2.0f);
		const FBox Bounds(FVector(-250.0, -50.0, -350.0), FVector(180.0, 220.0, 640.0));

		FIntVector MinCell, MaxCell;
		Lattice.WorldBoundsToCellRange(Bounds, MinCell, MaxCell);

		for (int32 X = -10; X <= 10; ++X)
		{
			for (int32 Y = -10; Y <= 10; ++Y)
			{
				for (int32 Z = -10; Z <= 10; ++Z)
				{
					const FIntVector Cell(X, Y, Z);
					if (!Bounds.IsInside(Lattice.GridToWorld(Cell)))
					{
						continue;
					}

					const bool bCovered =
						Cell.X >= MinCell.X && Cell.X <= MaxCell.X &&
						Cell.Y >= MinCell.Y && Cell.Y <= MaxCell.Y &&
						Cell.Z >= MinCell.Z && Cell.Z <= MaxCell.Z;
					if (!bCovered)
					{
						AddError(FString::Printf(TEXT("Клетка %s внутри бокса, но не попала в рамку [%s .. %s]"),
							*Cell.ToString(), *MinCell.ToString(), *MaxCell.ToString()));
						return false;
					}
				}
			}
		}
	}

	{
		// Виртуальный GridToWorld() сетки обязан совпадать с инлайновым у её
		// решётки. Именно это делает законным приём "взять GetLattice() один раз
		// перед горячим циклом и дальше не платить за виртуальный вызов" - если
		// кто-то оптимизирует одно и забудет другое, разойдутся картинка и
		// выделение, а причина будет неочевидна.
		FDenseCellGrid Grid(FLatticeTransform::MakeOrthogonal(100.0f, 2.0f), 16);
		const FLatticeTransform& Lattice = Grid.GetLattice();

		for (int32 Index = -20; Index <= 20; Index += 3)
		{
			const FIntVector Cell(Index, -Index, Index * 2);
			if (!Grid.GridToWorld(Cell).Equals(Lattice.GridToWorld(Cell)))
			{
				AddError(FString::Printf(TEXT("Виртуальный GridToWorld разошёлся с инлайновым на клетке %s"), *Cell.ToString()));
				return false;
			}
		}

		// Габарит чанка - вектор: на растянутой решётке чанк коробка, а не куб,
		// и вызывающие, берущие из него радиус, обязаны брать максимум.
		const FVector ChunkExtent = Grid.GetChunkWorldExtent();
		if (!FMath::IsNearlyEqual(ChunkExtent.Z, ChunkExtent.X * 2.0))
		{
			AddError(FString::Printf(TEXT("Габарит чанка %s не отражает растяжение по Z"), *ChunkExtent.ToString()));
			return false;
		}
	}

	{
		// Вьюха чанков обязана давать ЦЕНТР чанка. Поправка на центр переехала
		// из отдельного скалярного поля в Origin решётки и стала покомпонентной -
		// на растянутой решётке скаляр был бы верен лишь по одной оси.
		constexpr int32 ChunkSize = 16;
		const FLatticeTransform CellLattice = FLatticeTransform::MakeOrthogonal(100.0f, 2.0f);
		const FVector CellExtent = CellLattice.GetCellWorldExtent();
		const FVector ChunkExtent = CellExtent * static_cast<double>(ChunkSize);

		const FChunkGridView ChunkView(ChunkExtent, CellExtent, TArray<FIntVector>{ FIntVector(1, -2, 3) });

		const FIntVector ChunkCoord(1, -2, 3);
		// Чанк занимает клетки [C*ChunkSize .. C*ChunkSize + ChunkSize-1], его
		// центр - середина между мировыми позициями крайних из них.
		const FVector FirstCell = CellLattice.GridToWorld(ChunkCoord * ChunkSize);
		const FVector LastCell = CellLattice.GridToWorld(ChunkCoord * ChunkSize + FIntVector(ChunkSize - 1));
		const FVector ExpectedCenter = (FirstCell + LastCell) * 0.5;

		if (!ChunkView.GridToWorld(ChunkCoord).Equals(ExpectedCenter, 0.01))
		{
			AddError(FString::Printf(TEXT("Центр чанка %s: вьюха дала %s, ожидалось %s"),
				*ChunkCoord.ToString(), *ChunkView.GridToWorld(ChunkCoord).ToString(), *ExpectedCenter.ToString()));
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLatticeElongatedDodecahedronFacesTest,
	"CellularAutomata.Lattice.ElongatedDodecahedronFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FLatticeElongatedDodecahedronFacesTest::RunTest(const FString& Parameters)
{
	const TArray<FIntVector> Offsets = BuildLatticeNeighborOffsets(ELatticeNeighborhood::ElongatedDodecahedron12);

	// 12 граней = 12 соседей: одна грань на соседа - это определение ячейки
	// Вороного, а не пожелание.
	if (Offsets.Num() != 12)
	{
		AddError(FString::Printf(TEXT("Ожидалось 12 смещений, получено %d"), Offsets.Num()));
		return false;
	}

	TSet<FIntVector> Unique(Offsets);
	if (Unique.Num() != Offsets.Num())
	{
		AddError(TEXT("В наборе есть повторяющиеся смещения"));
		return false;
	}

	// Влезает и в шейдерный массив на 26, и в 32-битные маски Birth/Survival.
	if (Offsets.Num() > 26)
	{
		AddError(TEXT("Набор не влезает в шейдерный массив на 26 смещений"));
		return false;
	}

	// Замкнутость на ОЦК-подрешётке: смещение обязано сохранять условие "все
	// три координаты одной чётности", иначе структура уйдёт с подрешётки за
	// одно поколение и форма клетки перестанет иметь смысл.
	for (const FIntVector& Offset : Offsets)
	{
		// Чётность через &1, а не %2: у отрицательных %2 даёт -1 (знак идёт за
		// делимым), и проверка "== 1" молча не сработала бы.
		const int32 ParityX = Offset.X & 1;
		if ((Offset.Y & 1) != ParityX || (Offset.Z & 1) != ParityX)
		{
			AddError(FString::Printf(TEXT("Смещение %s уводит с ОЦК-подрешётки"), *Offset.ToString()));
			return false;
		}
	}

	// Дальность 2 - из-за дальних осей (+-2,0,0). Гало GPU-пачки считается
	// именно отсюда, и заниженное молча теряет пограничные клетки.
	const FCellularAutomatonRule Rule(TArray<int32>{ 1 }, TArray<int32>{ 1 }, Offsets);
	if (Rule.GetNeighborExtent() != 2)
	{
		AddError(FString::Printf(TEXT("Дальность набора %d, ожидалось 2"), Rule.GetNeighborExtent()));
		return false;
	}

	// ГЛАВНАЯ проверка, ради которой тест и существует: набор - это именно
	// ГРАНИ ячейки Вороного при растяжении по Z вдвое, а не произвольная
	// выборка. Грань к соседу V существует тогда и только тогда, когда середина
	// отрезка до него ближе к нулю, чем к любому другому узлу подрешётки.
	// Заодно это фиксирует и порог sqrt(2), и то, что грани к (0,0,+-2) уже нет.
	constexpr double ZScale = 2.0;
	const FLatticeTransform Lattice = FLatticeTransform::MakeOrthogonal(1.0f, static_cast<float>(ZScale));

	// Все узлы подрешётки в окрестности - против них и проверяем.
	TArray<FIntVector> Nodes;
	for (int32 X = -4; X <= 4; ++X)
	{
		for (int32 Y = -4; Y <= 4; ++Y)
		{
			for (int32 Z = -4; Z <= 4; ++Z)
			{
				if (X == 0 && Y == 0 && Z == 0)
				{
					continue;
				}
				const int32 ParityX = X & 1;
				if ((Y & 1) != ParityX || (Z & 1) != ParityX)
				{
					continue;
				}
				Nodes.Emplace(X, Y, Z);
			}
		}
	}

	auto HasFace = [&Lattice, &Nodes](const FIntVector& Candidate)
	{
		const FVector Midpoint = Lattice.GridToWorld(Candidate) * 0.5;
		const double DistToOrigin = Midpoint.SizeSquared();
		for (const FIntVector& Node : Nodes)
		{
			if (Node == Candidate)
			{
				continue;
			}
			// Строго ближе - касание в вершине гранью не считается.
			if (FVector::DistSquared(Midpoint, Lattice.GridToWorld(Node)) < DistToOrigin - UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				return false;
			}
		}
		return true;
	};

	for (const FIntVector& Offset : Offsets)
	{
		if (!HasFace(Offset))
		{
			AddError(FString::Printf(TEXT("У смещения %s нет грани: середина отрезка ближе к другому узлу"), *Offset.ToString()));
			return false;
		}
	}

	// Встречная проверка - без неё тест прошёл бы и на наборе "все 14 соседей
	// ОЦК": при растяжении вдвое грани к (0,0,+-2) НЕТ, и именно поэтому 14
	// превращается в 12.
	for (const FIntVector& Excluded : { FIntVector(0, 0, 2), FIntVector(0, 0, -2) })
	{
		if (HasFace(Excluded))
		{
			AddError(FString::Printf(TEXT("У смещения %s грань есть, хотя при растяжении по Z вдвое её быть не должно"), *Excluded.ToString()));
			return false;
		}
		if (Offsets.Contains(Excluded))
		{
			AddError(FString::Printf(TEXT("Смещение %s не должно входить в набор"), *Excluded.ToString()));
			return false;
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
