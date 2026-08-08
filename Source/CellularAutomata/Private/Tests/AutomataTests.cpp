#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Automata/Capture/CellRasterizer.h"
#include "Automata/Editing/CellClipboard.h"
#include "Automata/Editing/CellEditJournal.h"
#include "Automata/Generation/CellArrayModifier.h"
#include "Automata/Generation/StateGenerators.h"
#include "Automata/Grid/CellShapePresets.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Grid/LatticeTransform.h"
#include "Automata/Simulation/RulePresets.h"
#include "Automata/Meshing/ChunkGridView.h"
#include "Automata/Persistence/AutomatonStateSerializer.h"
#include "Automata/Rendering/CellVisibilityFilter.h"
#include "Automata/Rendering/ColorRamp.h"
#include "Automata/Selection/CellSelection.h"
#include "Core/PlayerController/HotkeyRegistry.h"
#include "GameFramework/InputSettings.h"
#include "Automata/Simulation/LatticeNeighborhood.h"
#include "Automata/Simulation/CellAging.h"
#include "Automata/Simulation/CellDecay.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/RuleStringParser.h"
#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"
#include "Automata/Simulation/ComputeStrategy/GpuComputeStrategy.h"
#include "Automata/Sonification/SonificationCurve.h"
#include "Math/RandomStream.h"
#include "Orchestration/GenerationHistory.h"
#include "Orchestration/StabilityWindow.h"
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
		TestEqual(TEXT("правило построило офсеты по окрестности"), Vn2Rule.GetNeighborOffsets().Num(), 24);
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
			{ ENeighborhood::PlanarMoore,         TEXT("PM") },
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
		const int32 TypeCount = static_cast<int32>(EStateGeneratorType::LifePattern) + 1;

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCellShapePresetTableTest,
	"CellularAutomata.CellShape.PresetTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

/** Стережёт три связи, которые появились вместе с тумблером формы и без которых
 *  он врёт молча.
 *
 *  Первая: перечисление ECellShape и таблица CellShapePresets - два списка одних
 *  и тех же пяти фигур, и разойтись они могут только в одну сторону (форма без
 *  строки в таблице). Тогда SetCellShape() получает INDEX_NONE, тумблер
 *  откатывается, а причина видна лишь в логе.
 *
 *  Вторая: SyncCellShapeFromLatticeFields() узнаёт форму по тройке
 *  (фильтр чётности, соседство-списком, растяжение по Z). Если две применимые
 *  формы совпадут по всем трём полям, загрузка .casave начнёт опознавать не ту -
 *  с чужим множителем меша и чужим ассетом, то есть со щелями в картинке.
 *
 *  Третья: FRulePreset::RequiredCellShape обязан ссылаться на форму, которую
 *  можно применить. Привязка к гексагональной призме означала бы пресет
 *  правила, который при каждом применении честно пишет в лог отказ. */
bool FCellShapePresetTableTest::RunTest(const FString& Parameters)
{
	const TArray<FCellShapePreset>& Presets = CellShapePresets::GetAll();

	// NumEnums() считает и скрытый _MAX, отсюда -1.
	const UEnum* ShapeEnum = StaticEnum<ECellShape>();
	const int32 ShapeCount = ShapeEnum ? ShapeEnum->NumEnums() - 1 : 0;
	if (ShapeCount <= 0)
	{
		AddError(TEXT("Не удалось получить UEnum для ECellShape"));
		return false;
	}

	if (Presets.Num() != ShapeCount)
	{
		AddError(FString::Printf(TEXT("Форм в таблице %d, значений в ECellShape %d - списки разошлись"),
			Presets.Num(), ShapeCount));
		return false;
	}

	for (int32 It = 0; It < ShapeCount; ++It)
	{
		const ECellShape Shape = static_cast<ECellShape>(ShapeEnum->GetValueByIndex(It));
		const int32 Index = CellShapePresets::IndexOf(Shape);
		if (Index == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("Формы %s нет в таблице пресетов"), *ShapeEnum->GetNameStringByIndex(It)));
			return false;
		}
		if (Presets[Index].Shape != Shape)
		{
			AddError(FString::Printf(TEXT("IndexOf(%s) дал строку с формой %d"),
				*ShapeEnum->GetNameStringByIndex(It), static_cast<int32>(Presets[Index].Shape)));
			return false;
		}
	}

	// Ключ распознавания уникален среди ПРИМЕНИМЫХ форм. Гексагональная призма
	// из проверки исключена намеренно: её поля решётки совпадают с кубическими
	// (скошенность живёт не в них), и ровно поэтому
	// SyncCellShapeFromLatticeFields() её пропускает.
	for (int32 It = 0; It < Presets.Num(); ++It)
	{
		if (CellShapePresets::RequiresShearedLattice(Presets[It]))
		{
			continue;
		}

		for (int32 Other = It + 1; Other < Presets.Num(); ++Other)
		{
			if (CellShapePresets::RequiresShearedLattice(Presets[Other]))
			{
				continue;
			}

			if (Presets[It].ParityFilter == Presets[Other].ParityFilter
				&& Presets[It].NeighborhoodShape == Presets[Other].NeighborhoodShape
				&& FMath::IsNearlyEqual(Presets[It].LatticeZScale, Presets[Other].LatticeZScale, 0.01f))
			{
				AddError(FString::Printf(TEXT("Формы '%s' и '%s' неразличимы по полям решётки - обратный поиск формы опознает не ту"),
					*Presets[It].Name, *Presets[Other].Name));
				return false;
			}
		}
	}

	// Одна грань на соседа - определение ячейки Вороного. Выбор набора здесь
	// повторяет BuildRule()/BuildNeighborOffsetsForAnalysis() (список формы
	// главнее оболочки), потому что проверяется именно то, что увидит
	// симуляция, а не то, что записано в поле FaceCount.
	for (const FCellShapePreset& Preset : Presets)
	{
		if (CellShapePresets::RequiresShearedLattice(Preset))
		{
			continue;
		}

		const TArray<FIntVector> LatticeOffsets = BuildLatticeNeighborOffsets(Preset.NeighborhoodShape);
		const int32 NeighborCount = LatticeOffsets.Num() > 0
			? LatticeOffsets.Num()
			: FCellularAutomatonRule::BuildNeighborOffsets(Preset.Neighborhood).Num();

		if (NeighborCount != Preset.FaceCount)
		{
			AddError(FString::Printf(TEXT("У формы '%s' %d граней, а соседей %d - рост не совпадёт с видимыми контактами"),
				*Preset.Name, Preset.FaceCount, NeighborCount));
			return false;
		}
	}

	// Привязки пресетов правил к решётке.
	for (const FRulePreset& RulePreset : RulePresets::GetAll())
	{
		if (!RulePreset.bRequiresCellShape)
		{
			continue;
		}

		const int32 Index = CellShapePresets::IndexOf(RulePreset.RequiredCellShape);
		if (Index == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("Пресет правила '%s' требует форму %d, которой нет в таблице"),
				*RulePreset.Name, static_cast<int32>(RulePreset.RequiredCellShape)));
			return false;
		}

		if (CellShapePresets::RequiresShearedLattice(Presets[Index]))
		{
			AddError(FString::Printf(TEXT("Пресет правила '%s' требует форму '%s', которую применить нельзя"),
				*RulePreset.Name, *Presets[Index].Name));
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCellEditJournalTest,
	"CellularAutomata.Editing.EditJournal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FCellEditJournalTest::RunTest(const FString& Parameters)
{
	constexpr int32 ChunkSize = 16;

	// Отмена возвращает состояние ЦЕЛИКОМ, а не один бит "жива". Возраст красит
	// клетку, фаза угасания влияет на само правило - вернуть клетку с нулём в
	// том и другом значит вернуть не ту клетку (см. doc-comment FCellEdit).
	{
		FDenseCellGrid Grid(100.0f, ChunkSize, /*bEnableDecay=*/true);

		const FIntVector Old(1, 2, 3);      // старая живая клетка
		const FIntVector Fresh(4, 5, 6);    // только что родившаяся
		const FIntVector Decaying(7, 8, 9); // не живая, но угасающая

		Grid.SetAliveWithAge(Old, 200);
		Grid.SetAliveWithAge(Fresh, 0);
		Grid.SetDecayState(Decaying, 3);

		// В набор нарочно попадает и угасающая клетка, и никогда не жившая:
		// удалять там нечего, и в записи их быть не должно - иначе журнал рос
		// бы на размер выделения, а не правки.
		const TArray<FIntVector> ToDelete = { Old, Fresh, Decaying, FIntVector(50, 50, 50) };

		FCellEditRecord Record = CellEditJournal::MakeDeleteRecord(Grid, ToDelete, /*Generation=*/7);
		TestEqual(TEXT("в запись попали только живые клетки"), Record.Edits.Num(), 2);
		TestEqual(TEXT("запись помнит своё поколение"), Record.Generation, (int64)7);

		CellEditJournal::ApplyForward(Grid, Record);
		TestFalse(TEXT("старая клетка удалена"), Grid.IsAlive(Old));
		TestFalse(TEXT("свежая клетка удалена"), Grid.IsAlive(Fresh));
		TestEqual(TEXT("удаление рукой не оставляет угасания"), (int32)Grid.GetDecayState(Old), 0);

		CellEditJournal::ApplyInverse(Grid, Record);
		TestTrue(TEXT("старая клетка вернулась"), Grid.IsAlive(Old));
		TestEqual(TEXT("вместе со своим возрастом"), (int32)Grid.GetAge(Old), 200);
		TestTrue(TEXT("свежая клетка вернулась"), Grid.IsAlive(Fresh));
		TestEqual(TEXT("её возраст остался нулевым"), (int32)Grid.GetAge(Fresh), 0);
		TestEqual(TEXT("угасающая клетка не пострадала"), (int32)Grid.GetDecayState(Decaying), 3);
	}

	// Добавление - зеркало удаления, и отмена обязана убрать ровно то, что оно
	// поставило, не тронув то, что там уже было живо.
	{
		FDenseCellGrid Grid(100.0f, ChunkSize);

		const FIntVector Existing(0, 0, 0);
		const FIntVector Added(1, 1, 1);
		Grid.SetAliveWithAge(Existing, 42);

		FCellEditRecord Record = CellEditJournal::MakeAddRecord(Grid, { Existing, Added }, /*Generation=*/0);
		TestEqual(TEXT("уже живая клетка в запись не попала"), Record.Edits.Num(), 1);

		CellEditJournal::ApplyForward(Grid, Record);
		TestTrue(TEXT("клетка добавлена"), Grid.IsAlive(Added));
		TestEqual(TEXT("поставленная рукой клетка молодая"), (int32)Grid.GetAge(Added), 0);

		CellEditJournal::ApplyInverse(Grid, Record);
		TestFalse(TEXT("добавленная клетка убрана"), Grid.IsAlive(Added));
		TestTrue(TEXT("соседняя живая клетка не тронута"), Grid.IsAlive(Existing));
		TestEqual(TEXT("и её возраст тоже"), (int32)Grid.GetAge(Existing), 42);
	}

	// Журнал как СЦЕНАРИЙ: две правки на разных поколениях, применённые по
	// порядку, дают то же самое, что дали они же вживую. Это то свойство, на
	// котором держится откат поколения после ручных правок (см.
	// AAutomataOrchestrator::StepBackward()).
	{
		FDenseCellGrid Live(100.0f, ChunkSize);
		FDenseCellGrid Replay(100.0f, ChunkSize);

		const TArray<FIntVector> Seed = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(2, 0, 0) };
		for (const FIntVector& Cell : Seed)
		{
			Live.SetAlive(Cell, true);
			Replay.SetAlive(Cell, true);
		}

		TArray<FCellEditRecord> Journal;
		Journal.Add(CellEditJournal::MakeDeleteRecord(Live, { FIntVector(1, 0, 0) }, 3));
		CellEditJournal::ApplyForward(Live, Journal.Last());
		Journal.Add(CellEditJournal::MakeAddRecord(Live, { FIntVector(5, 5, 5) }, 8));
		CellEditJournal::ApplyForward(Live, Journal.Last());

		for (const FCellEditRecord& Record : Journal)
		{
			CellEditJournal::ApplyForward(Replay, Record);
		}

		TestEqual(TEXT("воспроизведение даёт то же число клеток"), Replay.Num(), Live.Num());
		TestFalse(TEXT("удалённая клетка удалена и в воспроизведении"), Replay.IsAlive(FIntVector(1, 0, 0)));
		TestTrue(TEXT("добавленная клетка добавлена и в воспроизведении"), Replay.IsAlive(FIntVector(5, 5, 5)));

		TestEqual(TEXT("вес журнала считается по клеткам"), CellEditJournal::TotalCells(Journal), (int64)2);

		// Срез по поколению - откат назад за правку выбрасывает её из сценария,
		// иначе следующий откат воспроизвёл бы её снова.
		CellEditJournal::TrimAfter(Journal, 5);
		TestEqual(TEXT("правка позже точки отката выброшена"), Journal.Num(), 1);
		TestEqual(TEXT("правка до неё осталась"), Journal.Last().Generation, (int64)3);

		CellEditJournal::TrimAfter(Journal, 3);
		TestEqual(TEXT("правка ровно на точке отката остаётся"), Journal.Num(), 1);

		CellEditJournal::TrimAfter(Journal, 2);
		TestEqual(TEXT("уход за неё чистит журнал"), Journal.Num(), 0);
	}

	// ПЕРЕНОС набора (гизмо выделения) - одна запись вместо пары
	// "удалить + добавить", и вся сложность в ПЕРЕСЕЧЕНИИ старого и нового: при
	// сдвиге меньше габарита часть клеток остаётся на месте, и наивная пара
	// записей стёрла бы то, что вторая обязана была увидеть живым.
	{
		FDenseCellGrid Grid(100.0f, ChunkSize);
		// Ряд из трёх клеток; сдвиг на 1 по X оставляет две из них на месте.
		const TArray<FIntVector> Row = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(2, 0, 0) };
		for (const FIntVector& Cell : Row)
		{
			Grid.SetAlive(Cell, true);
		}
		Grid.SetAge(FIntVector(0, 0, 0), 40); // чтобы отмена вернула именно ЕГО

		const int32 CountBefore = Grid.Num();
		FCellEditRecord Move = CellEditJournal::MakeMoveRecord(Grid, Row, FIntVector(1, 0, 0), /*Generation=*/7);

		// В записи только края: две клетки посередине как были живыми, так и
		// остаются - изменения там нет, и хранить его значило бы записать
		// несуществующую правку (плюс обнулить им возраст на ровном месте).
		TestEqual(TEXT("перенос ряда на 1 задевает только края"), Move.Edits.Num(), 2);

		CellEditJournal::ApplyForward(Grid, Move);
		TestEqual(TEXT("перенос не меняет числа клеток"), Grid.Num(), CountBefore);
		TestFalse(TEXT("исходный край освободился"), Grid.IsAlive(FIntVector(0, 0, 0)));
		TestTrue(TEXT("новый край занят"), Grid.IsAlive(FIntVector(3, 0, 0)));

		// И главное: отмена возвращает ВСЁ, включая возраст клетки, которая
		// была стёрта переносом.
		CellEditJournal::ApplyInverse(Grid, Move);
		TestEqual(TEXT("отмена переноса возвращает число клеток"), Grid.Num(), CountBefore);
		TestTrue(TEXT("отмена вернула исходный край"), Grid.IsAlive(FIntVector(0, 0, 0)));
		TestFalse(TEXT("отмена убрала новый край"), Grid.IsAlive(FIntVector(3, 0, 0)));
		TestEqual(TEXT("отмена вернула возраст, а не обнулила его"), (int32)Grid.GetAge(FIntVector(0, 0, 0)), 40);

		// Сдвиг больше габарита - пересечения нет, задеты все шесть клеток.
		FCellEditRecord FarMove = CellEditJournal::MakeMoveRecord(Grid, Row, FIntVector(10, 0, 0), 7);
		TestEqual(TEXT("перенос без пересечения задевает оба набора целиком"), FarMove.Edits.Num(), 6);

		// Нулевой сдвиг - не действие: пустая запись, которую RecordEdit()
		// отсеет, а не "перенос на месте", отменяемый впустую.
		FCellEditRecord NoMove = CellEditJournal::MakeMoveRecord(Grid, Row, FIntVector::ZeroValue, 7);
		TestEqual(TEXT("нулевой сдвиг даёт пустую запись"), NoMove.Edits.Num(), 0);
	}

	return true;
}

namespace
{
	/** Синтетическая история: значение задаётся функцией от НОМЕРА поколения,
	 *  шаг по X произвольный. Именно от номера, а не от индекса, - иначе тест
	 *  на неравномерный шаг проверял бы сам себя. */
	TArray<FGenerationSample> MakeHistory(TFunctionRef<int32(int64)> Value,
		int64 FirstGeneration, int64 LastGeneration, int64 Stride)
	{
		TArray<FGenerationSample> History;
		for (int64 Generation = FirstGeneration; Generation <= LastGeneration; Generation += Stride)
		{
			FGenerationSample Sample;
			Sample.Generation = Generation;
			Sample.AliveCount = Value(Generation);
			History.Add(Sample);
		}
		return History;
	}

	/** Настройки, у которых окно накрывает всю историю целиком: почти все
	 *  проверки ниже про саму математику, а не про выбор окна. */
	FSonificationParams WideWindowParams()
	{
		FSonificationParams Params;
		Params.WindowGenerations = 1000000;
		Params.MinWindowSamples = 2;
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSonificationCurveTest,
	"CellularAutomata.Sonification.Curve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FSonificationCurveTest::RunTest(const FString& Parameters)
{
	const FSonificationParams Wide = WideWindowParams();

	// Удвоение за пять поколений - это ln2/5 е-фолдов на поколение. Проверка
	// того, что наклон меряется в правильных единицах: без неё все остальные
	// пороги были бы подогнаны под неизвестно что.
	const double ExpectedDoublingSlope = FMath::Loge(2.0) / 5.0;
	auto Doubling = [](int64 Generation) -> int32
	{
		return (int32)FMath::RoundToInt(100000.0 * FMath::Pow(2.0, (double)Generation / 5.0));
	};

	{
		const TArray<FGenerationSample> History = MakeHistory(Doubling, 0, 60, 1);
		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(History, Wide);

		TestTrue(TEXT("экспоненциальный рост измерим"), Features.bValid);
		TestEqual(TEXT("наклон равен ln2/5 на поколение"), (double)Features.LogSlope, ExpectedDoublingSlope, 1e-4);
		TestTrue(TEXT("у чистой экспоненты изгиба нет"), FMath::Abs(Features.Bend) < 0.05f);
		TestEqual(TEXT("форма - рост"), (int32)Features.Shape, (int32)ESonificationShape::Growth);
	}

	{
		// ТО ЖЕ САМОЕ, но замеры стоят вчетверо реже и с вырезанной серединой.
		// Это главное обещание всей подсистемы: окно живёт по номеру поколения,
		// а не по индексу в массиве, поэтому дыры от StepsPerRender, GPU-батча
		// и быстрого шага на измерение не влияют.
		TArray<FGenerationSample> Sparse = MakeHistory(Doubling, 0, 60, 7);
		Sparse.RemoveAt(2, 3);

		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(Sparse, Wide);
		TestEqual(TEXT("дыры в замерах наклон не меняют"), (double)Features.LogSlope, ExpectedDoublingSlope, 1e-4);
	}

	{
		// Второе обещание: звук описывает ФОРМУ кривой, а не размер сетки. Две
		// истории отличаются населением в тысячу раз и обязаны звучать одинаково.
		const TArray<FGenerationSample> Small = MakeHistory(
			[](int64 Generation) { return (int32)FMath::RoundToInt(1000.0 * FMath::Pow(2.0, (double)Generation / 5.0)); }, 0, 40, 1);
		const TArray<FGenerationSample> Large = MakeHistory(
			[](int64 Generation) { return (int32)FMath::RoundToInt(1000000.0 * FMath::Pow(2.0, (double)Generation / 5.0)); }, 0, 40, 1);

		const FSonificationFeatures SmallFeatures = SonificationCurve::ComputeFeatures(Small, Wide);
		const FSonificationFeatures LargeFeatures = SonificationCurve::ComputeFeatures(Large, Wide);

		TestEqual(TEXT("наклон не зависит от масштаба населения"), (double)SmallFeatures.LogSlope, (double)LargeFeatures.LogSlope, 1e-3);
		TestEqual(TEXT("изгиб тоже"), (double)SmallFeatures.Bend, (double)LargeFeatures.Bend, 1e-2);
	}

	{
		// ЛОВУШКА: в лог-координатах линейный рост выглядит как насыщение -
		// наклон положительный, убывающий, изгиб отрицательный. Различает их
		// только показатель роста, и без этой проверки регресс классификации
		// прошёл бы незамеченным.
		const TArray<FGenerationSample> History = MakeHistory(
			[](int64 Generation) { return (int32)(50 * Generation); }, 1, 100, 1);
		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(History, Wide);

		TestTrue(TEXT("линейный рост растёт"), Features.LogSlope > 0.0f);
		TestTrue(TEXT("и в логарифме гнётся вниз"), Features.Bend < 0.0f);
		TestEqual(TEXT("показатель роста линейной фигуры равен единице"), (double)Features.GrowthExponent, 1.0, 1e-3);
		TestEqual(TEXT("но это РОСТ, а не насыщение"), (int32)Features.Shape, (int32)ESonificationShape::Growth);
	}

	{
		// Настоящее насыщение: выход на потолок. Отличается от линейного роста
		// именно показателем - структура перестаёт расти вовсе.
		const TArray<FGenerationSample> History = MakeHistory(
			[](int64 Generation) { return (int32)FMath::RoundToInt(10000.0 * (1.0 - FMath::Exp(-(double)Generation / 20.0))); }, 1, 200, 1);
		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(History, Wide);

		TestTrue(TEXT("насыщение гнётся вниз заметно"), Features.Bend < -0.15f);
		TestTrue(TEXT("показатель роста упал ниже линейного"), Features.GrowthExponent < 0.9f);
		TestEqual(TEXT("форма - насыщение"), (int32)Features.Shape, (int32)ESonificationShape::Saturation);
	}

	{
		// Разгон: быстрее экспоненты, наклон сам растёт.
		const TArray<FGenerationSample> History = MakeHistory(
			[](int64 Generation) { return (int32)FMath::RoundToInt(FMath::Exp((double)(Generation * Generation) / 1000.0)); }, 0, 100, 1);
		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(History, Wide);

		TestTrue(TEXT("разгон гнётся вверх"), Features.Bend > 0.15f);
		TestEqual(TEXT("форма - взрывной рост"), (int32)Features.Shape, (int32)ESonificationShape::Explosive);
	}

	{
		// Обвал: в сто раз за три поколения.
		const TArray<FGenerationSample> History = MakeHistory(
			[](int64 Generation) { return (int32)FMath::RoundToInt(10000.0 * FMath::Pow(0.01, (double)Generation / 3.0)); }, 0, 3, 1);
		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(History, Wide);

		TestEqual(TEXT("наклон обвала - ln(0.01)/3"), (double)Features.LogSlope, FMath::Loge(0.01) / 3.0, 1e-2);
		TestEqual(TEXT("форма - обвал"), (int32)Features.Shape, (int32)ESonificationShape::Collapse);
	}

	{
		// Осциллятор: население никуда не пришло, но всё это время двигалось.
		// "Топчется на месте" и "стоит на месте" обязаны звучать по-разному -
		// иначе кипящее равновесие было бы неотличимо от мёртвого.
		const TArray<FGenerationSample> History = MakeHistory(
			[](int64 Generation) { return (int32)FMath::RoundToInt(1000.0 + 500.0 * FMath::Sin((double)Generation / 3.0)); }, 0, 120, 1);
		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(History, Wide);

		TestTrue(TEXT("осциллятор никуда не растёт"), FMath::Abs(Features.LogSlope) < 0.01f);
		TestTrue(TEXT("но движения в нём много"), Features.Activity > 0.05f);
		TestTrue(TEXT("путь много больше перемещения"), Features.Oscillation01 > 0.8f);
		TestEqual(TEXT("форма - колебание"), (int32)Features.Shape, (int32)ESonificationShape::Oscillation);
	}

	{
		// Вымирание. Прямая страховка от ln(0): одна минус бесконечность увела
		// бы в NaN всё, что считается дальше, и звук замолчал бы навсегда без
		// единого сообщения.
		TArray<FGenerationSample> History = MakeHistory(
			[](int64 Generation) { return (int32)FMath::Max<int64>(0, 1000 - 100 * Generation); }, 0, 12, 1);
		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(History, Wide);

		TestEqual(TEXT("на пустой сетке население ноль"), Features.Population01, 0.0f);
		TestEqual(TEXT("форма - вымерло"), (int32)Features.Shape, (int32)ESonificationShape::Extinct);
		TestTrue(TEXT("наклон конечен"), FMath::IsFinite(Features.LogSlope));
		TestTrue(TEXT("изгиб конечен"), FMath::IsFinite(Features.Bend));
		TestTrue(TEXT("кривизна конечна"), FMath::IsFinite(Features.LogCurvature));
		TestTrue(TEXT("активность конечна"), FMath::IsFinite(Features.Activity));
		TestTrue(TEXT("колебание конечно"), FMath::IsFinite(Features.Oscillation01));
		TestTrue(TEXT("показатель роста конечен"), FMath::IsFinite(Features.GrowthExponent));
	}

	{
		// "Мерить не по чему" - это НЕ измеренный нулевой рост. Тот же контракт,
		// что у GenerationHistory::FitGrowthExponent(), возвращающего 0.0.
		const TArray<FGenerationSample> Empty;
		const FSonificationFeatures EmptyFeatures = SonificationCurve::ComputeFeatures(Empty, Wide);
		TestFalse(TEXT("пустая история непригодна"), EmptyFeatures.bValid);
		TestEqual(TEXT("и формы у неё нет"), (int32)EmptyFeatures.Shape, (int32)ESonificationShape::Idle);

		const TArray<FGenerationSample> Single = MakeHistory([](int64) { return 500; }, 7, 7, 1);
		const FSonificationFeatures SingleFeatures = SonificationCurve::ComputeFeatures(Single, Wide);
		TestFalse(TEXT("одного замера мало"), SingleFeatures.bValid);
		TestEqual(TEXT("наклон при этом ноль, а не мусор"), SingleFeatures.LogSlope, 0.0f);
		TestEqual(TEXT("форма - нечего мерить"), (int32)SingleFeatures.Shape, (int32)ESonificationShape::Idle);
	}

	{
		// Все замеры на ОДНОМ поколении. Случай не выдуманный: NoteRendered()
		// правит последний замер на месте, а рендер дёргается на каждое
		// движение камеры при включённом срезе, так что на паузе окно вполне
		// схлопывается в одну точку по X.
		TArray<FGenerationSample> Degenerate;
		for (int32 Index = 0; Index < 5; ++Index)
		{
			FGenerationSample Sample;
			Sample.Generation = 42;
			Sample.AliveCount = 1000 + Index;
			Degenerate.Add(Sample);
		}

		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(Degenerate, Wide);
		TestFalse(TEXT("вырожденное по X окно непригодно"), Features.bValid);
		TestEqual(TEXT("без деления на ноль"), Features.LogSlope, 0.0f);
		TestTrue(TEXT("и без NaN"), FMath::IsFinite(Features.Bend));
	}

	{
		// Пол по числу замеров. При StepsPerRender в сотни окно, заданное в
		// поколениях, не накрывает ни одного замера - и обязано расшириться
		// назад, иначе мерить будет нечего при живых данных.
		FSonificationParams Narrow;
		Narrow.WindowGenerations = 64;
		Narrow.MinWindowSamples = 8;

		const TArray<FGenerationSample> Sparse = MakeHistory(
			[](int64 Generation) { return (int32)(1000 + Generation); }, 0, 256 * 20, 256);

		const FSonificationFeatures Features = SonificationCurve::ComputeFeatures(Sparse, Narrow);
		TestEqual(TEXT("окно расширилось до минимума замеров"), Features.SampleCount, 8);
		TestTrue(TEXT("и измерение состоялось"), Features.bValid);
	}

	{
		// Сглаживание обязано зависеть от ВРЕМЕНИ, а не от того, как нарезаны
		// кадры: здесь они плавают от восьми миллисекунд до секунды с лишним.
		// Полугрупповое свойство exp - единственное, что это гарантирует.
		const float AfterOneTau = SonificationCurve::SmoothTowards(0.0f, 1.0f, 0.5f, 0.5f);
		TestEqual(TEXT("за одну постоянную времени пройдено 1-1/e"), (double)AfterOneTau, 1.0 - 1.0 / UE_DOUBLE_EULERS_NUMBER, 1e-4);

		const float NoStep = SonificationCurve::SmoothTowards(0.25f, 1.0f, 0.5f, 0.0f);
		TestEqual(TEXT("нулевой шаг ничего не меняет"), NoStep, 0.25f);

		const float OneStep = SonificationCurve::SmoothTowards(0.0f, 1.0f, 0.5f, 0.4f);
		const float TwoHalves = SonificationCurve::SmoothTowards(
			SonificationCurve::SmoothTowards(0.0f, 1.0f, 0.5f, 0.2f), 1.0f, 0.5f, 0.2f);
		TestEqual(TEXT("два полушага равны одному целому"), (double)TwoHalves, (double)OneStep, 1e-6);

		const float NoSmoothing = SonificationCurve::SmoothTowards(0.0f, 1.0f, 0.0f, 0.016f);
		TestEqual(TEXT("нулевая постоянная времени - это отсутствие сглаживания"), NoSmoothing, 1.0f);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCellVisibilityFilterTest,
	"CellularAutomata.Rendering.VisibilityFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FCellVisibilityFilterTest::RunTest(const FString& Parameters)
{
	// Этот фильтр существует ровно потому, что раньше его логика стояла двумя
	// копиями - в BuildCellRenderData() и ComputeVisibleCellsBounds(), - и
	// инвариант между ними держался комментарием. Тесты здесь на то, что
	// копией больше не является и что границы заданы однозначно.

	{
		// Выключенный срез обязан пропускать ВСЁ, включая точки, которые при
		// включённом не прошли бы. Мимо этого легко промахнуться: у
		// выключенного среза Origin/Forward остаются значениями по умолчанию,
		// и скалярное произведение по ним осмысленного ответа не даёт.
		CellVisibility::FFilter Off;
		TestTrue(TEXT("выключенный срез пропускает начало координат"), Off.PassesSlice(FVector::ZeroVector));
		TestTrue(TEXT("выключенный срез пропускает далёкую точку"), Off.PassesSlice(FVector(1e6, -1e6, 1e6)));
	}

	{
		// Взгляд вдоль +X, срез на расстоянии 1000 толщиной 200 - то есть
		// глубины от 900 до 1100 включительно.
		const CellVisibility::FFilter Slice = CellVisibility::MakeSliceFilter(
			true, FVector::ZeroVector, FVector::ForwardVector, 1000.0f, 200.0f);

		TestEqual(TEXT("ближняя граница это расстояние минус половина толщины"), Slice.SliceMinDepth, 900.0);
		TestEqual(TEXT("дальняя граница это расстояние плюс половина толщины"), Slice.SliceMaxDepth, 1100.0);

		TestTrue(TEXT("центр среза внутри"), Slice.PassesSlice(FVector(1000.0, 0.0, 0.0)));
		// Границы включительные - ровно так вели себя обе прежние копии
		// (проверка была "Depth < Min || Depth > Max"), и от этого зависит,
		// не мигает ли клетка ровно на краю среза.
		TestTrue(TEXT("ближняя граница входит в срез"), Slice.PassesSlice(FVector(900.0, 0.0, 0.0)));
		TestTrue(TEXT("дальняя граница входит в срез"), Slice.PassesSlice(FVector(1100.0, 0.0, 0.0)));
		TestFalse(TEXT("ближе среза - мимо"), Slice.PassesSlice(FVector(899.0, 0.0, 0.0)));
		TestFalse(TEXT("дальше среза - мимо"), Slice.PassesSlice(FVector(1101.0, 0.0, 0.0)));

		// Срез меряет глубину ВДОЛЬ ВЗГЛЯДА, а не расстояние до камеры:
		// сдвиг вбок глубины не меняет, и клетка остаётся в срезе. Если бы
		// вместо скалярного произведения стояла длина вектора, этот случай
		// отвалился бы - а на экране выглядел бы как срез, выгнутый сферой.
		TestTrue(TEXT("сдвиг поперёк взгляда глубину не меняет"), Slice.PassesSlice(FVector(1000.0, 5000.0, -5000.0)));

		// Позади камеры глубина отрицательна и в диапазон не попадает.
		TestFalse(TEXT("позади камеры - мимо"), Slice.PassesSlice(FVector(-1000.0, 0.0, 0.0)));
	}

	{
		// Ненулевое начало и взгляд не вдоль оси: глубина считается от
		// положения камеры, а не от мировой точки отсчёта.
		const FVector Origin(100.0, 200.0, 300.0);
		const FVector Forward = FVector(1.0, 1.0, 0.0).GetSafeNormal();
		const CellVisibility::FFilter Slice = CellVisibility::MakeSliceFilter(
			true, Origin, Forward, 100.0f, 20.0f);

		TestTrue(TEXT("точка ровно на глубине 100 от камеры внутри"), Slice.PassesSlice(Origin + Forward * 100.0));
		TestFalse(TEXT("точка на глубине 50 от камеры снаружи"), Slice.PassesSlice(Origin + Forward * 50.0));
		// Сама камера на глубине 0 - вне среза, стоящего на 100.
		TestFalse(TEXT("положение камеры вне собственного среза"), Slice.PassesSlice(Origin));
	}

	{
		// Выключенный фильтр возраста пропускает всё, и это НЕ то же самое,
		// что "выбран возраст 0": ноль - законный слой (только что родившиеся
		// клетки), и путать эти два состояния значит прятать самый заметный
		// слой при каждом сбросе фильтра.
		CellVisibility::FFilter NoAge;
		TestTrue(TEXT("выключенный фильтр пропускает возраст 0"), NoAge.PassesAge(0));
		TestTrue(TEXT("выключенный фильтр пропускает возраст 255"), NoAge.PassesAge(255));

		CellVisibility::FFilter Age;
		Age.bAgeFilterActive = true;
		Age.AgeMask.Init(false, 256);
		Age.AgeMask[0] = true;
		Age.AgeMask[7] = true;
		Age.AgeMask[255] = true;

		TestTrue(TEXT("возраст 0 проходит, когда выбран явно"), Age.PassesAge(0));
		TestTrue(TEXT("выбранный возраст в середине проходит"), Age.PassesAge(7));
		TestTrue(TEXT("верхний возраст проходит - маска покрывает все 256"), Age.PassesAge(255));
		TestFalse(TEXT("невыбранный возраст не проходит"), Age.PassesAge(1));
		TestFalse(TEXT("невыбранный возраст у верхней границы не проходит"), Age.PassesAge(254));
	}

	{
		// Два фильтра независимы: пройти надо оба. Проверяется потому, что в
		// цикле они стоят двумя отдельными continue, и порядок проверок там
		// выбран по цене, а не по смыслу.
		CellVisibility::FFilter Both = CellVisibility::MakeSliceFilter(
			true, FVector::ZeroVector, FVector::ForwardVector, 1000.0f, 200.0f);
		Both.bAgeFilterActive = true;
		Both.AgeMask.Init(false, 256);
		Both.AgeMask[3] = true;

		const FVector Inside(1000.0, 0.0, 0.0);
		const FVector Outside(0.0, 0.0, 0.0);

		TestTrue(TEXT("нужный возраст в срезе проходит оба"), Both.PassesAge(3) && Both.PassesSlice(Inside));
		TestFalse(TEXT("нужный возраст вне среза не проходит срез"), Both.PassesSlice(Outside));
		TestFalse(TEXT("чужой возраст в срезе не проходит возраст"), Both.PassesAge(4));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHotkeyRegistryTest,
	"CellularAutomata.Input.HotkeyRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FHotkeyRegistryTest::RunTest(const FString& Parameters)
{
	const TArray<HotkeyRegistry::FHotkeyDefault>& Defaults = HotkeyRegistry::GetDefaults();

	// Таблица индексируется значением EHotkey напрямую (KeyFor() берёт по
	// индексу), поэтому её длина и порядок - не оформление, а условие
	// корректности. Забытая строка при добавлении хоткея сдвинула бы ВСЕ
	// последующие клавиши на одну, и это выглядело бы как "половина раскладки
	// разъехалась", а не как отсутствующий хоткей.
	TestEqual(TEXT("в таблице ровно столько строк, сколько значений EHotkey"), Defaults.Num(), (int32)EHotkey::Count);

	for (int32 Index = 0; Index < Defaults.Num(); ++Index)
	{
		if (Defaults[Index].Hotkey != (EHotkey)Index)
		{
			AddError(FString::Printf(TEXT("строка %d таблицы описывает не то действие - порядок разъехался"), Index));
			break;
		}
	}

	// Ни одна клавиша по умолчанию не может быть пустой: EKeys::Invalid здесь
	// означал бы хоткей, который молча не работает.
	for (const HotkeyRegistry::FHotkeyDefault& Default : Defaults)
	{
		if (!Default.DefaultKey.IsValid())
		{
			AddError(FString::Printf(TEXT("у действия %s нет клавиши по умолчанию"), Default.ActionName));
		}
	}

	// Имена уникальны: два действия под одним именем в ini неразличимы, и
	// второе тихо получило бы клавишу первого.
	{
		TSet<FString> Names;
		for (const HotkeyRegistry::FHotkeyDefault& Default : Defaults)
		{
			bool bAlreadyThere = false;
			Names.Add(FString(Default.ActionName), &bAlreadyThere);
			if (bAlreadyThere)
			{
				AddError(FString::Printf(TEXT("имя действия %s встречается дважды"), Default.ActionName));
			}
		}
	}

	// Раскладка по умолчанию сама себе не противоречит. Пары, делящие клавишу
	// намеренно (Ctrl+Z против голой Z), помечены bModifierGuarded и проверкой
	// пропускаются - иначе предупреждение горело бы всегда, а такое не читают.
	{
		TArray<FKey> DefaultKeys;
		DefaultKeys.Reserve(Defaults.Num());
		for (const HotkeyRegistry::FHotkeyDefault& Default : Defaults)
		{
			DefaultKeys.Add(Default.DefaultKey);
		}

		const TMap<FKey, TArray<FName>> Conflicts = HotkeyRegistry::FindConflicts(DefaultKeys);
		for (const TPair<FKey, TArray<FName>>& Conflict : Conflicts)
		{
			TArray<FString> Names;
			for (const FName& Name : Conflict.Value)
			{
				Names.Add(Name.ToString());
			}
			AddError(FString::Printf(TEXT("клавиша %s назначена нескольким действиям: %s"),
				*Conflict.Key.ToString(), *FString::Join(Names, TEXT(", "))));
		}
	}

	// Проверка самой проверки: на выдуманном конфликте FindConflicts() обязана
	// сработать, иначе пустой результат выше не значил бы ничего.
	{
		TArray<FKey> Colliding;
		Colliding.Init(EKeys::F, Defaults.Num());
		const TMap<FKey, TArray<FName>> Conflicts = HotkeyRegistry::FindConflicts(Colliding);
		TestEqual(TEXT("подстроенный конфликт находится"), Conflicts.Num(), 1);
	}

	// Фильтр по возрасту берётся арифметикой AgeFilter0 + Digit прямо в
	// InputKey(), так что десять значений обязаны идти подряд и по порядку.
	for (int32 Digit = 0; Digit < 10; ++Digit)
	{
		const int32 Index = (int32)EHotkey::AgeFilter0 + Digit;
		if (!Defaults.IsValidIndex(Index))
		{
			AddError(TEXT("значения AgeFilter0..9 выходят за таблицу"));
			break;
		}
		TestEqual(*FString::Printf(TEXT("AgeFilter%d идёт %d-м по счёту от AgeFilter0"), Digit, Digit),
			(int32)Defaults[Index].Hotkey - (int32)EHotkey::AgeFilter0, Digit);
	}

	// ResolveKeys() без конфига обязана вернуть ровно значения по умолчанию:
	// это то, что делает проект работоспособным при пустом ini.
	{
		const TArray<FKey> Resolved = HotkeyRegistry::ResolveKeys();
		TestEqual(TEXT("разрешённая раскладка той же длины, что таблица"), Resolved.Num(), Defaults.Num());

		// Под тестами UInputSettings несёт то, что лежит в DefaultInput.ini, -
		// то есть либо строку CA_*, либо ничего. В обоих случаях клавиша обязана
		// быть валидной: пустая означала бы, что действие осталось без клавиши.
		for (int32 Index = 0; Index < Resolved.Num(); ++Index)
		{
			if (!Resolved[Index].IsValid())
			{
				AddError(FString::Printf(TEXT("после разрешения у действия %s нет клавиши"), Defaults[Index].ActionName));
			}
		}

		// А это то, что отличает "клавиша прочитана из конфига" от "взято
		// значение по умолчанию": пока ini повторяет значения по умолчанию,
		// совпадение результата ничего не доказывает - механизм мог бы не
		// работать вовсе, и тест этого не заметил бы. Поэтому сверяемся с тем,
		// что реально лежит в UInputSettings: если строка CA_* там есть,
		// разрешённая клавиша обязана быть именно ей, а не значением из кода.
		if (const UInputSettings* Settings = GetDefault<UInputSettings>())
		{
			TMap<FName, FKey> Configured;
			for (const FInputActionKeyMapping& Mapping : Settings->GetActionMappings())
			{
				if (!Configured.Contains(Mapping.ActionName))
				{
					Configured.Add(Mapping.ActionName, Mapping.Key);
				}
			}

			int32 CheckedFromConfig = 0;
			for (int32 Index = 0; Index < Defaults.Num(); ++Index)
			{
				const FName ActionName(Defaults[Index].ActionName);
				const FKey* ConfiguredKey = Configured.Find(ActionName);
				if (!ConfiguredKey || !ConfiguredKey->IsValid())
				{
					continue;
				}

				++CheckedFromConfig;
				TestEqual(*FString::Printf(TEXT("%s взят из конфига"), Defaults[Index].ActionName),
					Resolved[Index].ToString(), ConfiguredKey->ToString());
			}

			// Конфиг может отсутствовать целиком - это законно (см. doc-comment
			// EHotkey), но тогда стоит знать, что проверка выше ничего не
			// проверила, а не считать её пройденной.
			if (CheckedFromConfig == 0)
			{
				AddWarning(TEXT("в DefaultInput.ini нет ни одной строки CA_* - раскладка целиком на значениях по умолчанию"));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLifePatternTest,
	"CellularAutomata.Generation.LifePattern",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FLifePatternTest::RunTest(const FString& Parameters)
{
	// Перенос двумерной жизни в 3D (EStateGeneratorType::LifePattern плюс
	// правило 5,7/6). Проверяется не таблица координат - её проверяет глаз, -
	// а АРИФМЕТИКА, на которой перенос держится: в двухслойной призме клетка
	// видит своих соседей дважды плюс собственного двойника, поэтому N соседей
	// в 2D превращаются в 2N+1 у живой клетки и 2N у мёртвой. Именно отсюда
	// берутся 5,7/6 из B3/S23, и если равенство не выполняется, правило,
	// записанное в пресет, просто неверно.

	const ELifePattern Patterns[] = {
		ELifePattern::Glider, ELifePattern::LightweightSpaceship,
		ELifePattern::Pulsar, ELifePattern::GosperGliderGun
	};

	for (ELifePattern Pattern : Patterns)
	{
		FStateGeneratorParams Params;
		Params.Type = EStateGeneratorType::LifePattern;
		Params.LifePattern = Pattern;
		Params.Thickness = 2;
		Params.bAnalyzeNeighborCounts = false;

		TArray<FIntVector> Cells;
		StateGenerators::FGenerateStats Stats;
		FString Error;
		if (!StateGenerators::Generate(Params, /*Seed=*/0, MAX_int64, Cells, Stats, Error))
		{
			AddError(FString::Printf(TEXT("генератор паттерна отказал: %s"), *Error));
			continue;
		}

		const int32 PatternIndex = static_cast<int32>(Pattern);

		// Оценка обязана быть точной: паттерн - таблица фиксированной длины на
		// число слоёв, оценивать тут нечего.
		TestEqual(*FString::Printf(TEXT("паттерн %d: оценка совпала с фактом"), PatternIndex),
			(int64)Cells.Num(), StateGenerators::EstimateCellCount(Params));

		// Ровно два слоя, и они одинаковые: выдавливание - это копия, а не
		// новая фигура.
		TSet<FIntPoint> LayerA, LayerB;
		TSet<int32> Layers;
		for (const FIntVector& Cell : Cells)
		{
			Layers.Add(Cell.Z);
			(Cell.Z == Cells[0].Z ? LayerA : LayerB).Add(FIntPoint(Cell.X, Cell.Y));
		}
		TestEqual(*FString::Printf(TEXT("паттерн %d: ровно два слоя"), PatternIndex), Layers.Num(), 2);
		TestTrue(*FString::Printf(TEXT("паттерн %d: слои совпадают"), PatternIndex),
			LayerA.Num() == LayerB.Num() && LayerA.Includes(LayerB));

		// И главное. Для КАЖДОЙ клетки плоского паттерна считаем соседей по
		// Moore-8 в 2D и по Moore-26 в выдавленной призме, и требуем 2N+1 у
		// живых, 2N у пустых. Это ровно то тождество, из которого выведено
		// правило 5,7/6, - проверенное на настоящих паттернах, а не на бумаге.
		const TSet<FIntVector> Live(Cells);
		const TSet<FIntPoint> Flat(LayerA);
		const int32 ZLow = FMath::Min(Cells[0].Z, Cells.Last().Z);

		TSet<FIntPoint> Checked;
		for (const FIntPoint& Cell : Flat)
		{
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				for (int32 dx = -1; dx <= 1; ++dx)
				{
					const FIntPoint Probe(Cell.X + dx, Cell.Y + dy);
					if (Checked.Contains(Probe))
					{
						continue;
					}
					Checked.Add(Probe);

					int32 FlatNeighbors = 0;
					for (int32 ny = -1; ny <= 1; ++ny)
					{
						for (int32 nx = -1; nx <= 1; ++nx)
						{
							if ((nx != 0 || ny != 0) && Flat.Contains(FIntPoint(Probe.X + nx, Probe.Y + ny)))
							{
								++FlatNeighbors;
							}
						}
					}

					// Считаем в НИЖНЕМ слое призмы: у него сосед только сверху,
					// и это тот самый случай, для которого выведена формула.
					const FIntVector Probe3D(Probe.X, Probe.Y, ZLow);
					int32 SpatialNeighbors = 0;
					for (int32 nz = -1; nz <= 1; ++nz)
					{
						for (int32 ny = -1; ny <= 1; ++ny)
						{
							for (int32 nx = -1; nx <= 1; ++nx)
							{
								if ((nx != 0 || ny != 0 || nz != 0)
									&& Live.Contains(FIntVector(Probe3D.X + nx, Probe3D.Y + ny, Probe3D.Z + nz)))
								{
									++SpatialNeighbors;
								}
							}
						}
					}

					const bool bAlive = Flat.Contains(Probe);
					const int32 Expected = bAlive ? (2 * FlatNeighbors + 1) : (2 * FlatNeighbors);
					if (SpatialNeighbors != Expected)
					{
						AddError(FString::Printf(
							TEXT("паттерн %d, клетка (%d,%d) %s: соседей в 2D %d, ожидалось в 3D %d, вышло %d"),
							PatternIndex, Probe.X, Probe.Y, bAlive ? TEXT("живая") : TEXT("пустая"),
							FlatNeighbors, Expected, SpatialNeighbors));
					}

					// То же самое под PlanarMoore - соседством, ради которого
					// оно всё и затевалось: там клетка видит только свой слой и
					// двух прямых соседей по Z, поэтому N+1 у живой и N у
					// мёртвой. Отсюда 3,4/3 из B3/S23.
					int32 PlanarNeighbors = 0;
					for (int32 nz = -1; nz <= 1; ++nz)
					{
						for (int32 ny = -1; ny <= 1; ++ny)
						{
							for (int32 nx = -1; nx <= 1; ++nx)
							{
								// Плоскость XY целиком плюс чистая ось Z - без
								// вертикальных диагоналей.
								const bool bInPlanarSet = (nz == 0 && (nx != 0 || ny != 0))
													   || (nz != 0 && nx == 0 && ny == 0);
								if (bInPlanarSet
									&& Live.Contains(FIntVector(Probe3D.X + nx, Probe3D.Y + ny, Probe3D.Z + nz)))
								{
									++PlanarNeighbors;
								}
							}
						}
					}

					const int32 ExpectedPlanar = bAlive ? (FlatNeighbors + 1) : FlatNeighbors;
					if (PlanarNeighbors != ExpectedPlanar)
					{
						AddError(FString::Printf(
							TEXT("паттерн %d, клетка (%d,%d) %s под PlanarMoore: ожидалось %d, вышло %d"),
							PatternIndex, Probe.X, Probe.Y, bAlive ? TEXT("живая") : TEXT("пустая"),
							ExpectedPlanar, PlanarNeighbors));
					}
				}
			}
		}
	}

	// ГЕРМЕТИЧНОСТЬ - то, ради чего заведено соседство PlanarMoore, и то, что
	// первая версия этого теста проверяла НЕВЕРНО. Считалось, что паттерн
	// остаётся плоским, пока у пустой клетки нет шести соседей, - и у ружья
	// Госпера выходило четыре, то есть "всё хорошо". В действительности клетка
	// над призмой видит N соседей ПЛЮС ДВОЙНИКА, живого когда позиция жива, так
	// что считать надо N + [жива]: у ружья это 6 (живая клетка с пятью
	// соседями), у пульсара тоже 6 (пустая с шестью). Оба обрастали вверх на
	// первом же шаге, и только глайдер с его пятёркой летел - ровно то, что и
	// наблюдалось вживую, пока ошибку не нашли.
	//
	// Поэтому проверяются ОБА соседства: под Moore-26 - фактический порог
	// каждого паттерна (знание, а не утверждение), под PlanarMoore - что
	// протечки нет вовсе.
	{
		struct FThresholdCase { ELifePattern Pattern; const TCHAR* Name; int32 ExpectedMoore; };
		const FThresholdCase Cases[] = {
			{ ELifePattern::Glider,               TEXT("глайдер"),      5 },
			{ ELifePattern::LightweightSpaceship, TEXT("LWSS"),         5 },
			{ ELifePattern::Pulsar,               TEXT("пульсар"),      6 },
			{ ELifePattern::GosperGliderGun,      TEXT("ружьё"),        6 },
		};

		for (const FThresholdCase& Case : Cases)
		{
			FStateGeneratorParams Params;
			Params.Type = EStateGeneratorType::LifePattern;
			Params.LifePattern = Case.Pattern;
			Params.Thickness = 2;
			Params.bAnalyzeNeighborCounts = false;

			TArray<FIntVector> Cells;
			StateGenerators::FGenerateStats Stats;
			FString Error;
			StateGenerators::Generate(Params, 0, MAX_int64, Cells, Stats, Error);

			TSet<FIntPoint> Flat;
			for (const FIntVector& Cell : Cells)
			{
				Flat.Add(FIntPoint(Cell.X, Cell.Y));
			}

			int32 MaxSeenFromAbove = 0;
			TSet<FIntPoint> Checked;
			for (const FIntPoint& Cell : Flat)
			{
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					for (int32 dx = -1; dx <= 1; ++dx)
					{
						const FIntPoint Probe(Cell.X + dx, Cell.Y + dy);
						if (Checked.Contains(Probe))
						{
							continue;
						}
						Checked.Add(Probe);

						int32 Neighbors = 0;
						for (int32 ny = -1; ny <= 1; ++ny)
						{
							for (int32 nx = -1; nx <= 1; ++nx)
							{
								if ((nx != 0 || ny != 0) && Flat.Contains(FIntPoint(Probe.X + nx, Probe.Y + ny)))
								{
									++Neighbors;
								}
							}
						}

						// Вот она, забытая единица: двойник под клеткой.
						const int32 SeenFromAbove = Neighbors + (Flat.Contains(Probe) ? 1 : 0);
						MaxSeenFromAbove = FMath::Max(MaxSeenFromAbove, SeenFromAbove);
					}
				}
			}

			AddInfo(FString::Printf(TEXT("%s: клетка над призмой видит максимум %d (Moore-26, рождение при 6); под PlanarMoore - максимум 1"),
				Case.Name, MaxSeenFromAbove));
			TestEqual(*FString::Printf(TEXT("%s: порог под Moore-26"), Case.Name), MaxSeenFromAbove, Case.ExpectedMoore);
		}
	}

	// И само утверждение о герметичности - прямым счётом по смещениям
	// PlanarMoore, а не рассуждением: сколько клеток двухслойной призмы видит
	// клетка НАД ней. Ответ обязан быть не больше единицы (двойник под собой), а
	// значит при B={3} рождение вне двух слоёв невозможно ни для какого
	// паттерна - в этом всё отличие от Moore-26.
	{
		const TArray<FIntVector> PlanarOffsets = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::PlanarMoore);
		TestEqual(TEXT("PlanarMoore - ровно 10 соседей"), PlanarOffsets.Num(), 10);

		int32 MaxFromAbove = 0;
		// Худший случай: сплошной двухслойный блок 5x5x2, плотнее любого
		// паттерна жизни. Клетка над ним всё равно не должна видеть больше
		// одного.
		TSet<FIntVector> Slab;
		for (int32 X = -2; X <= 2; ++X)
		{
			for (int32 Y = -2; Y <= 2; ++Y)
			{
				Slab.Add(FIntVector(X, Y, 0));
				Slab.Add(FIntVector(X, Y, 1));
			}
		}

		for (int32 X = -2; X <= 2; ++X)
		{
			for (int32 Y = -2; Y <= 2; ++Y)
			{
				const FIntVector Above(X, Y, 2);
				int32 Neighbors = 0;
				for (const FIntVector& Offset : PlanarOffsets)
				{
					if (Slab.Contains(Above + Offset))
					{
						++Neighbors;
					}
				}
				MaxFromAbove = FMath::Max(MaxFromAbove, Neighbors);
			}
		}

		TestEqual(TEXT("над сплошной призмой клетка видит ровно одного соседа - двойника"), MaxFromAbove, 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStabilityWindowTest,
	"CellularAutomata.Generation.StabilityWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FStabilityWindowTest::RunTest(const FString& Parameters)
{
	// Окно численности - дешёвый триггер детектора застоя (см.
	// AAutomataOrchestrator::bAutoReseedOnStasis). Цена ошибки несимметрична:
	// пропущенный застой стоит нескольких лишних секунд перебора, а ЛОЖНЫЙ
	// застой выбрасывает сид, который мог оказаться находкой, - поэтому
	// проверяется прежде всего то, что мешает окну срабатывать раньше времени.

	// ПЕРВОЕ: незрелое окно не стабильно НИКОГДА, даже если все замеры равны.
	// Иначе первые же два одинаковых замера в начале прогона объявили бы
	// застоем структуру, которая ещё не начала разворачиваться.
	{
		FStabilityWindow Window;
		Window.Reset(8);
		for (int32 Index = 0; Index < 7; ++Index)
		{
			Window.Push(100);
			TestFalse(TEXT("незрелое окно не объявляет застой"), Window.IsStable(0));
		}
		Window.Push(100);
		TestTrue(TEXT("созревшее окно из равных замеров - застой"), Window.IsStable(0));
	}

	// ВТОРОЕ: одно изменение внутри окна снимает застой - и снимает его до тех
	// пор, пока не выйдет за пределы окна. Это то, что отличает "структура
	// стоит" от "структура почти стоит".
	{
		FStabilityWindow Window;
		Window.Reset(4);
		Window.Push(10); Window.Push(10); Window.Push(11); Window.Push(10);
		TestFalse(TEXT("разброс в окне - не застой"), Window.IsStable(0));

		// Пропихиваем изменение за край окна.
		Window.Push(10); Window.Push(10); Window.Push(10);
		TestTrue(TEXT("изменение вышло за окно - снова застой"), Window.IsStable(0));
	}

	// ТРЕТЬЕ: допуск - это разброс, а не "равно соседу". Осциллятор при
	// StepsPerRender > 1 попадает в замеры разными фазами, так что численность
	// честно скачет между двумя значениями; строгое равенство соседей не поймало
	// бы ни одного осциллятора.
	{
		FStabilityWindow Window;
		Window.Reset(6);
		for (int32 Index = 0; Index < 6; ++Index)
		{
			Window.Push(Index % 2 == 0 ? 40 : 43);
		}
		TestFalse(TEXT("колебание шире допуска - не застой"), Window.IsStable(2));
		TestTrue(TEXT("колебание в пределах допуска - застой"), Window.IsStable(3));
	}

	// ЧЕТВЁРТОЕ: Clear() забывает накопленное, но не ёмкость - им пользуется
	// каждый новый сид, и окно после него обязано созревать заново, а не
	// объявлять застой на первом же замере.
	{
		FStabilityWindow Window;
		Window.Reset(3);
		Window.Push(5); Window.Push(5); Window.Push(5);
		TestTrue(TEXT("окно созрело"), Window.IsStable(0));

		Window.Clear();
		TestFalse(TEXT("после Clear() окно снова незрелое"), Window.IsStable(0));
		Window.Push(5); Window.Push(5);
		TestFalse(TEXT("двух замеров из трёх всё ещё мало"), Window.IsStable(0));
		Window.Push(5);
		TestTrue(TEXT("третий замер снова даёт застой"), Window.IsStable(0));
	}

	// ПЯТОЕ, вырожденное: ёмкость меньше двух бессмысленна (разброс не из чего
	// считать) и обязана подниматься до двух, а не давать окно из одного замера,
	// которое стабильно всегда.
	{
		FStabilityWindow Window;
		Window.Reset(1);
		Window.Push(7);
		TestFalse(TEXT("окно из одного замера не объявляет застой"), Window.IsStable(0));
		Window.Push(7);
		TestTrue(TEXT("минимальное окно - два замера"), Window.IsStable(0));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCellClipboardTest,
	"CellularAutomata.Editing.Clipboard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FCellClipboardTest::RunTest(const FString& Parameters)
{
	// Буфер обмена и привязка вставки к грани (CellClipboard). Ошибка здесь не
	// падает и не логируется - она видна только после вставки, тем, что фигура
	// легла не туда, наполовину в стену.

	// ПЕРВОЕ: нормализация. Кусок, вырезанный за тысячу клеток от нуля, обязан
	// стать буфером вокруг нуля - иначе вставка задавала бы положение сдвигом
	// от места копирования, а оно к моменту вставки не значит ничего.
	{
		TArray<FIntVector> Cells = {
			FIntVector(1000, -2000, 500),
			FIntVector(1004, -2000, 500),
			FIntVector(1002, -1996, 508),
		};
		const int32 CountBefore = Cells.Num();
		CellClipboard::Normalize(Cells);

		TestEqual(TEXT("нормализация не теряет клетки"), Cells.Num(), CountBefore);

		FIntVector Min, Max;
		TestTrue(TEXT("габарит буфера считается"), CellClipboard::ComputeBounds(Cells, Min, Max));
		// Допуск в клетку: середина габарита может лежать на полуцелой
		// координате, и floor-деление опускает её вниз.
		TestTrue(TEXT("центр буфера у нуля"),
			FMath::Abs(Min.X + Max.X) <= 1 && FMath::Abs(Min.Y + Max.Y) <= 1 && FMath::Abs(Min.Z + Max.Z) <= 1);

		// Форма обязана уцелеть: нормализация - это сдвиг, а не пересбор.
		TestEqual(TEXT("габарит по X сохранился"), Max.X - Min.X, 4);
		TestEqual(TEXT("габарит по Z сохранился"), Max.Z - Min.Z, 8);
	}

	// ВТОРОЕ, и ради этого тест написан: вдоль нормали буфер ПРИЖИМАЕТСЯ к
	// грани, а не центрируется на ней. Фигура высотой в 11 клеток, положенная на
	// пол, обязана встать НА него - при центрировании пять её нижних слоёв ушли
	// бы под поверхность, и заметно это только глазом, после вставки.
	{
		const FIntVector BufferMin(-2, -3, -5);
		const FIntVector BufferMax(2, 3, 5);
		const FIntVector BaseCell(10, 20, 30); // первая свободная клетка над гранью

		// Кладём на грань, смотрящую вверх.
		{
			const FIntVector Origin = CellClipboard::ComputePasteOrigin(BufferMin, BufferMax, BaseCell, FIntVector(0, 0, 1));
			TestEqual(TEXT("низ буфера сел ровно на грань"), Origin.Z + BufferMin.Z, BaseCell.Z);
			TestEqual(TEXT("поперёк нормали буфер центрирован (X)"), Origin.X, BaseCell.X);
			TestEqual(TEXT("поперёк нормали буфер центрирован (Y)"), Origin.Y, BaseCell.Y);
		}

		// И симметрично - подвешиваем под грань, смотрящую вниз.
		{
			const FIntVector Origin = CellClipboard::ComputePasteOrigin(BufferMin, BufferMax, BaseCell, FIntVector(0, 0, -1));
			TestEqual(TEXT("верх буфера сел ровно под грань"), Origin.Z + BufferMax.Z, BaseCell.Z);
		}

		// Боковая грань - та же прижимка, но по другой оси; проверяется отдельно,
		// потому что перепутанная ось здесь выглядит правдоподобно.
		{
			const FIntVector Origin = CellClipboard::ComputePasteOrigin(BufferMin, BufferMax, BaseCell, FIntVector(1, 0, 0));
			TestEqual(TEXT("край буфера сел на боковую грань"), Origin.X + BufferMin.X, BaseCell.X);
			TestEqual(TEXT("по Z при боковой грани - центрирование"), Origin.Z, BaseCell.Z);
		}
	}

	// ТРЕТЬЕ: нулевая нормаль (промах либо камера внутри клетки) значит
	// "прижимать не к чему" - буфер просто центрируется на точке.
	{
		const FIntVector Origin = CellClipboard::ComputePasteOrigin(
			FIntVector(-2, -2, -2), FIntVector(2, 2, 2), FIntVector(7, 8, 9), FIntVector::ZeroValue);
		TestEqual(TEXT("без грани буфер центрируется на точке"), Origin, FIntVector(7, 8, 9));
	}

	// ЧЕТВЁРТОЕ: Place() - это сдвиг всего набора и ничего больше.
	{
		const TArray<FIntVector> Buffer = { FIntVector(0, 0, 0), FIntVector(1, 2, 3) };
		TArray<FIntVector> Placed;
		CellClipboard::Place(Buffer, FIntVector(10, 10, 10), Placed);

		TestEqual(TEXT("вставка не теряет клетки"), Placed.Num(), 2);
		TestTrue(TEXT("клетки сдвинуты на якорь"),
			Placed.Contains(FIntVector(10, 10, 10)) && Placed.Contains(FIntVector(11, 12, 13)));
	}

	// ПЯТОЕ: поворот на 90. Проверяется не "координаты стали такими-то", а три
	// свойства, каждое из которых ловит свой класс ошибки.
	{
		// Фигура намеренно НЕсимметричная по всем трём осям: на симметричной
		// перепутанный знак или ось не видны вовсе.
		const TArray<FIntVector> Original = {
			FIntVector(0, 0, 0), FIntVector(3, 0, 0), FIntVector(0, 1, 0), FIntVector(0, 0, 5), FIntVector(3, 1, 5),
		};

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			// ЧЕТЫРЕ поворота - тождество, бит в бит. Это то, что делает
			// поворот на решётке точным: любое округление накапливалось бы, и
			// круг не сошёлся бы. Сравнение как МНОЖЕСТВО - порядок клеток
			// внутри буфера ничего не значит.
			TArray<FIntVector> Cells = Original;
			for (int32 Turn = 0; Turn < 4; ++Turn)
			{
				CellClipboard::Rotate90(Cells, Axis, /*bClockwise=*/true);
			}

			TArray<FIntVector> NormalizedOriginal = Original;
			CellClipboard::Normalize(NormalizedOriginal);
			TestEqual(*FString::Printf(TEXT("ось %d: четыре поворота не теряют клеток"), Axis), Cells.Num(), Original.Num());
			TestTrue(*FString::Printf(TEXT("ось %d: четыре поворота дают исходную фигуру"), Axis),
				TSet<FIntVector>(Cells).Includes(TSet<FIntVector>(NormalizedOriginal)));

			// Обратный поворот - действительно обратный, а не ещё один прямой:
			// у него своя формула, и перепутанный знак виден только здесь.
			TArray<FIntVector> ThereAndBack = Original;
			CellClipboard::Rotate90(ThereAndBack, Axis, /*bClockwise=*/true);
			CellClipboard::Rotate90(ThereAndBack, Axis, /*bClockwise=*/false);
			TestTrue(*FString::Printf(TEXT("ось %d: поворот туда-обратно возвращает фигуру"), Axis),
				TSet<FIntVector>(ThereAndBack).Includes(TSet<FIntVector>(NormalizedOriginal)));

			// И анти-вакуумность: один поворот обязан что-то ИЗМЕНИТЬ. Без этого
			// реализация, не делающая ничего, прошла бы обе проверки выше.
			TArray<FIntVector> Once = Original;
			CellClipboard::Rotate90(Once, Axis, /*bClockwise=*/true);
			TestFalse(*FString::Printf(TEXT("ось %d: один поворот меняет фигуру"), Axis),
				TSet<FIntVector>(Once).Includes(TSet<FIntVector>(NormalizedOriginal)));
		}

		// Габариты обязаны МЕНЯТЬСЯ МЕСТАМИ - это отличает настоящий поворот от
		// зеркала, которое тоже прошло бы "четыре раза = тождество" на этой
		// фигуре. Вокруг Z меняются X и Y, Z остаётся.
		{
			TArray<FIntVector> Cells = Original;
			FIntVector MinBefore, MaxBefore;
			CellClipboard::ComputeBounds(Cells, MinBefore, MaxBefore);

			CellClipboard::Rotate90(Cells, /*Axis=*/2, /*bClockwise=*/true);

			FIntVector MinAfter, MaxAfter;
			CellClipboard::ComputeBounds(Cells, MinAfter, MaxAfter);
			TestEqual(TEXT("поворот вокруг Z: X стал прежним Y"), MaxAfter.X - MinAfter.X, MaxBefore.Y - MinBefore.Y);
			TestEqual(TEXT("поворот вокруг Z: Y стал прежним X"), MaxAfter.Y - MinAfter.Y, MaxBefore.X - MinBefore.X);
			TestEqual(TEXT("поворот вокруг Z: Z не тронут"), MaxAfter.Z - MinAfter.Z, MaxBefore.Z - MinBefore.Z);
		}

		// Буфер обязан оставаться у нуля: поворот идёт вокруг него, и центр
		// габарита с чётной стороной уезжает на полклетки. Без пересчёта буфер
		// отползал бы от курсора с каждым поворотом - по чуть-чуть, то есть
		// незаметно до десятого нажатия.
		{
			TArray<FIntVector> Cells = { FIntVector(0, 0, 0), FIntVector(1, 0, 0) }; // чётная сторона
			for (int32 Turn = 0; Turn < 8; ++Turn)
			{
				CellClipboard::Rotate90(Cells, /*Axis=*/2, /*bClockwise=*/true);

				FIntVector Min, Max;
				CellClipboard::ComputeBounds(Cells, Min, Max);
				TestTrue(TEXT("буфер не уползает от нуля при повторных поворотах"),
					FMath::Abs(Min.X + Max.X) <= 1 && FMath::Abs(Min.Y + Max.Y) <= 1);
			}
		}
	}

	// ШЕСТОЕ, вырожденное: пустой буфер не должен ни падать, ни выдумывать
	// габарит - ComputeBounds() честно отвечает false, а Normalize() ничего не
	// делает.
	{
		TArray<FIntVector> Empty;
		FIntVector Min, Max;
		TestFalse(TEXT("у пустого буфера габарита нет"), CellClipboard::ComputeBounds(Empty, Min, Max));
		CellClipboard::Normalize(Empty);
		TestEqual(TEXT("пустой буфер остаётся пустым"), Empty.Num(), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPickFaceNormalTest,
	"CellularAutomata.Selection.PickFaceNormal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FPickFaceNormalTest::RunTest(const FString& Parameters)
{
	// Нормаль грани, через которую луч вошёл в клетку (CellSelection::
	// PickCellAlongRay). На ней стоит "прилипание" при рисовании: новая клетка
	// ставится в HitCell + Normal, поэтому ошибка в знаке ставит клетку ВНУТРЬ
	// структуры, а ошибка в оси - сбоку от того места, куда целились.
	constexpr float CellSize = 100.0f;
	constexpr int32 ChunkSize = 8;
	FDenseCellGrid Grid(CellSize, ChunkSize);
	Grid.SetAlive(FIntVector(0, 0, 0), true);

	// Клетка (0,0,0) стоит в начале координат и занимает [-50, +50] по каждой
	// оси: GridToWorld() даёт ЦЕНТР клетки.
	struct FCase
	{
		FVector Origin;
		FVector Direction;
		FIntVector ExpectedNormal;
		const TCHAR* Name;
	};

	const FCase Cases[] = {
		{ FVector(500, 0, 0),  FVector(-1, 0, 0), FIntVector(1, 0, 0),  TEXT("справа") },
		{ FVector(-500, 0, 0), FVector(1, 0, 0),  FIntVector(-1, 0, 0), TEXT("слева") },
		{ FVector(0, 0, 500),  FVector(0, 0, -1), FIntVector(0, 0, 1),  TEXT("сверху") },
		{ FVector(0, 0, -500), FVector(0, 0, 1),  FIntVector(0, 0, -1), TEXT("снизу") },
		{ FVector(0, 500, 0),  FVector(0, -1, 0), FIntVector(0, 1, 0),  TEXT("сбоку по Y") },
	};

	for (const FCase& Case : Cases)
	{
		FIntVector HitCell;
		FIntVector Normal;
		if (!CellSelection::PickCellAlongRay(Grid, Case.Origin, Case.Direction, 10000.0, HitCell, Normal))
		{
			AddError(FString::Printf(TEXT("луч %s не нашёл клетку"), Case.Name));
			continue;
		}

		TestEqual(*FString::Printf(TEXT("клетка под лучом %s"), Case.Name), HitCell, FIntVector(0, 0, 0));
		TestEqual(*FString::Printf(TEXT("нормаль грани %s"), Case.Name), Normal, Case.ExpectedNormal);

		// Главное следствие, ради которого нормаль и нужна: клетка, поставленная
		// по ней, оказывается СНАРУЖИ, между камерой и попавшей клеткой. Знак
		// проверяется именно так, а не сравнением с константой: ошибка в знаке
		// ставит клетку внутрь структуры, и это то, что видно глазом.
		const FIntVector Placed = HitCell + Normal;
		const FVector PlacedWorld = Grid.GetLattice().GridToWorld(Placed);
		const FVector HitWorld = Grid.GetLattice().GridToWorld(HitCell);
		TestTrue(*FString::Printf(TEXT("новая клетка (%s) ближе к камере, чем та, в которую попали"), Case.Name),
			FVector::DistSquared(Case.Origin, PlacedWorld) < FVector::DistSquared(Case.Origin, HitWorld));
	}

	// Нормаль всегда по ОДНОЙ оси, даже когда луч идёт наискось: DDA шагает по
	// одной оси за итерацию, и диагональной "грани" не существует. Без этой
	// проверки реализация, складывающая нормали двух последних шагов, прошла бы
	// все случаи выше - они все осевые.
	{
		FIntVector HitCell;
		FIntVector Normal;
		const FVector Origin(400, 380, 360);
		if (CellSelection::PickCellAlongRay(Grid, Origin, -Origin.GetSafeNormal(), 10000.0, HitCell, Normal))
		{
			const int32 NonZero = (Normal.X != 0 ? 1 : 0) + (Normal.Y != 0 ? 1 : 0) + (Normal.Z != 0 ? 1 : 0);
			TestEqual(TEXT("у наклонного луча нормаль всё равно осевая"), NonZero, 1);
		}
		else
		{
			AddError(TEXT("наклонный луч не нашёл клетку"));
		}
	}

	// Нулевая нормаль - ЗНАЧИМЫЙ ответ: луч начался внутри самой клетки (камера
	// залетела в структуру), грани входа не существует. Вызывающий обязан
	// отличать это от осевой нормали - иначе клетка встала бы сама в себя.
	{
		FIntVector HitCell;
		FIntVector Normal;
		TestTrue(TEXT("луч изнутри клетки всё равно находит её"),
			CellSelection::PickCellAlongRay(Grid, FVector::ZeroVector, FVector(1, 0, 0), 10000.0, HitCell, Normal));
		TestEqual(TEXT("изнутри клетки грани входа нет"), Normal, FIntVector::ZeroValue);
	}

	// Промах не должен оставлять нормаль от прошлого вызова: она обнуляется в
	// начале, а не только при попадании.
	{
		FIntVector HitCell;
		FIntVector Normal(7, 7, 7);
		TestFalse(TEXT("луч мимо всего живого не находит клетку"),
			CellSelection::PickCellAlongRay(Grid, FVector(500, 500, 500), FVector(1, 1, 1).GetSafeNormal(), 10000.0, HitCell, Normal));
		TestEqual(TEXT("при промахе нормаль обнулена"), Normal, FIntVector::ZeroValue);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCellArrayModifierTest,
	"CellularAutomata.Generation.ArrayModifier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FCellArrayModifierTest::RunTest(const FString& Parameters)
{
	// Тираж (CellArrayModifier::Tile(), хоткей Ctrl+D). Источник везде один и
	// тот же - две клетки в ряд по X, то есть габарит (2,1,1): на нём видно
	// разницу между "габаритом" и "расстоянием между крайними клетками",
	// которая и есть главная ловушка этого модуля (шаг впритык равен 2, а не 1).
	const TArray<FIntVector> Source = { FIntVector(0, 0, 0), FIntVector(1, 0, 0) };

	TestEqual(TEXT("габарит двух клеток в ряд"), CellArrayModifier::ComputeSize(Source), FIntVector(2, 1, 1));

	// ПЕРВОЕ: Count = 1 по всем осям обязан быть тождеством. Это то, во что
	// упирается каждый случай "тираж настроен, но по этой оси не размножаем", и
	// сдвиг центрирования на нём обязан быть нулевым при любом шаге - иначе
	// набор незаметно уезжал бы от источника на ровном месте.
	{
		FCellArrayParams Params;
		Params.Count = FIntVector(1, 1, 1);
		Params.RelativeOffset = FVector(3.0, 3.0, 3.0); // заведомо ненулевой шаг
		Params.bCenterOnSource = true;

		TArray<FIntVector> Result;
		CellArrayModifier::Tile(Source, Params, Result);

		TestEqual(TEXT("Count=1 не меняет число клеток"), Result.Num(), Source.Num());
		TestTrue(TEXT("Count=1 не двигает клетки"),
			TSet<FIntVector>(Result).Includes(TSet<FIntVector>(Source)));
	}

	// ВТОРОЕ, и ради этого тест написан: RelativeOffset = 1 значит ВПРИТЫК -
	// копии касаются, но не наезжают и не оставляют щели. Три копии габарита 2
	// обязаны дать сплошной отрезок из шести клеток; наезд на клетку (шаг 1)
	// дал бы четыре, щель (шаг 3) - разрывы. Ни то, ни другое не видно по
	// числу клеток в логе, только по множеству.
	{
		FCellArrayParams Params;
		Params.Count = FIntVector(3, 1, 1);
		Params.RelativeOffset = FVector(1.0, 1.0, 1.0);
		Params.bCenterOnSource = false;

		TestEqual(TEXT("шаг впритык равен габариту"),
			CellArrayModifier::ComputeStep(FIntVector(2, 1, 1), Params), FIntVector(2, 1, 1));

		TArray<FIntVector> Result;
		CellArrayModifier::Tile(Source, Params, Result);

		const TSet<FIntVector> Unique(Result);
		TestEqual(TEXT("три копии по две клетки"), Result.Num(), 6);
		TestEqual(TEXT("наложений нет"), Unique.Num(), 6);
		for (int32 X = 0; X <= 5; ++X)
		{
			TestTrue(*FString::Printf(TEXT("клетка (%d,0,0) на месте"), X), Unique.Contains(FIntVector(X, 0, 0)));
		}
	}

	// ТРЕТЬЕ: ConstantOffset прибавляется к шагу поверх доли габарита - именно
	// этим задаётся зазор в одну пустую клетку, то есть разница между "копии
	// срослись в одно тело" и "стоят рядом" с точки зрения правила.
	{
		FCellArrayParams Params;
		Params.Count = FIntVector(2, 1, 1);
		Params.RelativeOffset = FVector(1.0, 1.0, 1.0);
		Params.ConstantOffset = FIntVector(1, 0, 0);
		Params.bCenterOnSource = false;

		TArray<FIntVector> Result;
		CellArrayModifier::Tile(Source, Params, Result);

		const TSet<FIntVector> Unique(Result);
		TestFalse(TEXT("между копиями пустая клетка"), Unique.Contains(FIntVector(2, 0, 0)));
		TestTrue(TEXT("вторая копия сдвинута на 3"), Unique.Contains(FIntVector(3, 0, 0)));
	}

	// ЧЕТВЁРТОЕ: центрирование. При нечётном Count центр тиража обязан совпасть
	// с центром источника ТОЧНО - это то, ради чего флаг существует (камера
	// смотрит на источник, и тираж не должен уезжать из кадра).
	{
		FCellArrayParams Params;
		Params.Count = FIntVector(3, 1, 1);
		Params.RelativeOffset = FVector(1.0, 1.0, 1.0);
		Params.bCenterOnSource = true;

		TArray<FIntVector> Result;
		CellArrayModifier::Tile(Source, Params, Result);

		int32 MinX = MAX_int32, MaxX = MIN_int32;
		for (const FIntVector& Cell : Result)
		{
			MinX = FMath::Min(MinX, Cell.X);
			MaxX = FMath::Max(MaxX, Cell.X);
		}
		// Источник занимает [0,1], центр 0.5; тираж обязан лечь на [-2,3] с тем
		// же центром.
		TestEqual(TEXT("центрированный тираж начинается на -2"), MinX, -2);
		TestEqual(TEXT("центрированный тираж кончается на 3"), MaxX, 3);
	}

	// ПЯТОЕ: оценка бюджета обязана быть верхней границей - на ней стоит отказ
	// ДО касания сетки, и заниженная оценка означала бы стёртое состояние вместо
	// честного отказа. Проверяется и на наезжающих копиях, где фактический
	// результат меньше оценки.
	{
		FCellArrayParams Params;
		Params.Count = FIntVector(2, 2, 2);
		Params.RelativeOffset = FVector(0.0, 0.0, 0.0); // все копии друг в друге

		TestEqual(TEXT("оценка - произведение Count на размер источника"),
			CellArrayModifier::EstimateCellCount(Source.Num(), Params), (int64)16);

		TArray<FIntVector> Result;
		CellArrayModifier::Tile(Source, Params, Result);
		TestTrue(TEXT("фактических уникальных клеток не больше оценки"),
			TSet<FIntVector>(Result).Num() <= 16);
	}

	// ШЕСТОЕ, вырожденные концы. Пустой источник не должен давать набор из
	// нулей, а Count <= 0 (структура доступна из Blueprint мимо ClampMin) обязан
	// читаться как 1: "ноль копий" означало бы молча стёртую сцену.
	{
		FCellArrayParams Params;
		Params.Count = FIntVector(0, -5, 1);

		TArray<FIntVector> Empty;
		TArray<FIntVector> Result;
		CellArrayModifier::Tile(Empty, Params, Result);
		TestEqual(TEXT("пустой источник - пустой результат"), Result.Num(), 0);

		CellArrayModifier::Tile(Source, Params, Result);
		TestEqual(TEXT("Count <= 0 читается как 1"), Result.Num(), Source.Num());
		TestEqual(TEXT("оценка при Count <= 0 тоже как при 1"),
			CellArrayModifier::EstimateCellCount(Source.Num(), Params), (int64)2);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveCenteringOffsetTest,
	"CellularAutomata.Persistence.CenteringOffset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FSaveCenteringOffsetTest::RunTest(const FString& Parameters)
{
	// Перенос паттерна в начало координат при записи .casave (см.
	// AutomatonStateSerializer::ComputeCenteringOffset()). Проверяются ровно те
	// три свойства, на которых он держится, - остальное в нём тривиально.

	// ПЕРВОЕ: центр габарита действительно приезжает к нулю. Проверка идёт на
	// узоре, СМЕЩЁННОМ далеко от нуля и НЕсимметричном по каждой оси, - это и
	// есть рабочий случай (рой вырос за тысячи клеток от начала координат), а
	// на симметричном вокруг нуля наборе ошибка знака была бы не видна.
	{
		const TArray<FIntVector> Cells = {
			FIntVector(1000, -3001, 40),
			FIntVector(1040, -2941, 40),
			FIntVector(1020, -2971, 100),
		};

		const FIntVector Offset = AutomatonStateSerializer::ComputeCenteringOffset(Cells);

		FIntVector Min(MAX_int32), Max(MIN_int32);
		for (const FIntVector& Cell : Cells)
		{
			const FIntVector Moved = Cell + Offset;
			Min.X = FMath::Min(Min.X, Moved.X); Max.X = FMath::Max(Max.X, Moved.X);
			Min.Y = FMath::Min(Min.Y, Moved.Y); Max.Y = FMath::Max(Max.Y, Moved.Y);
			Min.Z = FMath::Min(Min.Z, Moved.Z); Max.Z = FMath::Max(Max.Z, Moved.Z);
		}

		// Допуск в две клетки, а не ноль: сдвиг округляется до чётного (см.
		// ниже), и середина габарита сама может лежать на полуцелой координате.
		const FIntVector Center((Min.X + Max.X) / 2, (Min.Y + Max.Y) / 2, (Min.Z + Max.Z) / 2);
		if (FMath::Abs(Center.X) > 2 || FMath::Abs(Center.Y) > 2 || FMath::Abs(Center.Z) > 2)
		{
			AddError(FString::Printf(TEXT("центр после переноса (%d,%d,%d) - слишком далеко от нуля"),
				Center.X, Center.Y, Center.Z));
		}
	}

	// ВТОРОЕ, и ради этого тест написан: сдвиг ЧЁТНЫЙ по каждой оси. Нечётный
	// перенёс бы ГЦК/ОЦК-узор на соседнюю подрешётку (см. ECellParityFilter), и
	// поломка была бы молчаливой - файл открылся бы, картинка выглядела бы той
	// же, а первое же пересевание ушло бы мимо. Перебором смещений проверяется
	// именно это: где бы узор ни стоял, обе чётности - и сумма координат
	// (Even/Odd), и совпадение чётностей между координатами (SameParity) -
	// обязаны пережить перенос.
	for (int32 Shift = -3; Shift <= 3; ++Shift)
	{
		const TArray<FIntVector> Cells = {
			FIntVector(Shift, Shift, Shift),
			FIntVector(Shift + 7, Shift + 2, Shift + 5),
			FIntVector(Shift - 4, Shift + 9, Shift - 1),
		};

		const FIntVector Offset = AutomatonStateSerializer::ComputeCenteringOffset(Cells);

		if ((Offset.X & 1) != 0 || (Offset.Y & 1) != 0 || (Offset.Z & 1) != 0)
		{
			AddError(FString::Printf(TEXT("сдвиг (%d,%d,%d) нечётный - узор уедет на соседнюю подрешётку"),
				Offset.X, Offset.Y, Offset.Z));
			continue;
		}

		for (const FIntVector& Cell : Cells)
		{
			const FIntVector Moved = Cell + Offset;
			const int32 SumBefore = Cell.X + Cell.Y + Cell.Z;
			const int32 SumAfter = Moved.X + Moved.Y + Moved.Z;
			TestEqual(TEXT("чётность суммы координат пережила перенос"),
				FMath::Abs(SumAfter % 2), FMath::Abs(SumBefore % 2));

			const bool bSameParityBefore = ((Cell.X ^ Cell.Y) & 1) == 0 && ((Cell.Y ^ Cell.Z) & 1) == 0;
			const bool bSameParityAfter = ((Moved.X ^ Moved.Y) & 1) == 0 && ((Moved.Y ^ Moved.Z) & 1) == 0;
			TestEqual(TEXT("совпадение чётностей координат пережило перенос"), bSameParityAfter, bSameParityBefore);
		}
	}

	// ТРЕТЬЕ: пустой набор даёт нулевой сдвиг, а не обращение к Cells[0].
	// InitialStateCells пустым до сохранения не доходит (WriteStateToFile()
	// отказывается раньше), но функция публичная и обязана быть тотальной.
	{
		const TArray<FIntVector> Empty;
		TestEqual(TEXT("пустой набор - нулевой сдвиг"),
			AutomatonStateSerializer::ComputeCenteringOffset(Empty), FIntVector::ZeroValue);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
