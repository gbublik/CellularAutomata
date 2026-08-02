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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNeighborhoodRadiusTest,
	"CellularAutomata.Rules.NeighborhoodRadius",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FNeighborhoodRadiusTest::RunTest(const FString& Parameters)
{
	// Числа соседей - это и есть всё содержание радиуса, и каждое из них
	// упирается в конкретный потолок в другом месте кода: 24 у фон Неймана
	// радиуса 2 обязаны остаться <= 26 (шейдерный массив, см.
	// GpuComputeStrategy.cpp::MaxShaderNeighborOffsets) и < 32 (маски правила
	// там же). 124 у Moore радиуса 2 приведены не потому, что поддержаны, а
	// потому, что именно это число обе границы и ломает.
	const TArray<FIntVector> VN1 = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::VonNeumann, 1);
	const TArray<FIntVector> M1 = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::Moore, 1);
	const TArray<FIntVector> VN2 = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::VonNeumann, 2);
	const TArray<FIntVector> M2 = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::Moore, 2);

	const TArray<FIntVector> Edges = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::Edges, 1);
	const TArray<FIntVector> Corners = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::Corners, 1);
	const TArray<FIntVector> FacesEdges = FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::FacesEdges, 1);

	TestEqual(TEXT("фон Нейман радиуса 1 - 6 соседей"), VN1.Num(), 6);
	TestEqual(TEXT("Moore радиуса 1 - 26 соседей"), M1.Num(), 26);
	TestEqual(TEXT("фон Нейман радиуса 2 - 24 соседа"), VN2.Num(), 24);
	TestEqual(TEXT("Moore радиуса 2 - 124 соседа"), M2.Num(), 124);
	TestEqual(TEXT("рёбра - 12 соседей"), Edges.Num(), 12);
	TestEqual(TEXT("диагонали - 8 соседей"), Corners.Num(), 8);
	TestEqual(TEXT("грани+рёбра - 18 соседей"), FacesEdges.Num(), 18);

	// Формы обязаны быть именно РАЗБИЕНИЕМ куба 3x3x3, а не произвольными
	// наборами: грани+рёбра+диагонали дают ровно Moore, и ни одна пара из
	// трёх классов не пересекается. Проверяется множествами, а не числами -
	// 6+12+8=26 сошлось бы и при перепутанных классах.
	{
		TSet<FIntVector> Union;
		Union.Append(VN1);
		Union.Append(Edges);
		Union.Append(Corners);
		TestEqual(TEXT("грани+рёбра+диагонали в сумме дают весь куб 3x3x3"), Union.Num(), 26);

		bool bAllInMoore = true;
		for (const FIntVector& Offset : Union)
		{
			if (!M1.Contains(Offset))
			{
				bAllInMoore = false;
			}
		}
		TestTrue(TEXT("объединение классов совпадает с Moore"), bAllInMoore);

		TSet<FIntVector> FacesEdgesExpected;
		FacesEdgesExpected.Append(VN1);
		FacesEdgesExpected.Append(Edges);
		TestEqual(TEXT("грани+рёбра - это ровно грани плюс рёбра"), FacesEdgesExpected.Num(), FacesEdges.Num());

		bool bFacesEdgesMatch = true;
		for (const FIntVector& Offset : FacesEdges)
		{
			if (!FacesEdgesExpected.Contains(Offset))
			{
				bFacesEdgesMatch = false;
			}
		}
		TestTrue(TEXT("грани+рёбра не содержит ничего лишнего"), bFacesEdgesMatch);
	}

	// Структурное свойство, ради которого эти формы и добавлены: рёбра
	// сохраняют чётность суммы координат (решётка распадается на две
	// независимые подрешётки), диагонали переворачивают чётность каждой
	// координаты (на четыре). Если это сломается, формы станут просто
	// "Moore поменьше" и потеряют весь смысл.
	for (const FIntVector& Offset : Edges)
	{
		TestTrue(TEXT("ребро не меняет чётность суммы координат"), ((Offset.X + Offset.Y + Offset.Z) % 2) == 0);
	}
	for (const FIntVector& Offset : Corners)
	{
		TestTrue(TEXT("диагональ переворачивает чётность каждой координаты"),
			FMath::Abs(Offset.X) == 1 && FMath::Abs(Offset.Y) == 1 && FMath::Abs(Offset.Z) == 1);
	}

	// У форм радиуса нет - запрошенный игнорируется, а не растягивает набор.
	TestEqual(TEXT("рёбра с радиусом 2 дают тот же набор из 12"),
		FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood::Edges, 2).Num(), 12);

	TestTrue(TEXT("фон Нейман радиуса 2 влезает в шейдерный массив и в 32-битные маски"), VN2.Num() <= 26);

	// Радиус 1 обязан давать РОВНО прежний набор в прежнем порядке - иначе
	// появление радиуса могло бы незаметно сдвинуть уже сохранённые прогоны.
	// Порядок сам по себе ни на что не влияет (оба compute-пути только
	// суммируют по нему), но проверить дешевле, чем каждый раз доказывать.
	const TArray<FIntVector> ExpectedVN1 = {
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0),
		FIntVector(0, 0, 1), FIntVector(0, 0, -1)
	};
	TestTrue(TEXT("фон Нейман радиуса 1 - прежний набор в прежнем порядке"), VN1 == ExpectedVN1);

	TArray<FIntVector> ExpectedM1;
	for (int32 dx = -1; dx <= 1; ++dx)
	{
		for (int32 dy = -1; dy <= 1; ++dy)
		{
			for (int32 dz = -1; dz <= 1; ++dz)
			{
				if (dx != 0 || dy != 0 || dz != 0)
				{
					ExpectedM1.Add(FIntVector(dx, dy, dz));
				}
			}
		}
	}
	TestTrue(TEXT("Moore радиуса 1 - прежний набор в прежнем порядке"), M1 == ExpectedM1);

	// Метрики: фон Нейман - Манхэттен, Moore - Чебышёв. Проверяется не только
	// "не больше радиуса", но и что ни один офсет не повторяется и центра нет.
	auto CheckSet = [this](const TArray<FIntVector>& Offsets, int32 Radius, bool bMoore, const TCHAR* Label)
	{
		TSet<FIntVector> Unique;
		bool bMetricOk = true;
		for (const FIntVector& Offset : Offsets)
		{
			Unique.Add(Offset);
			const int32 Chebyshev = FMath::Max3(FMath::Abs(Offset.X), FMath::Abs(Offset.Y), FMath::Abs(Offset.Z));
			const int32 Manhattan = FMath::Abs(Offset.X) + FMath::Abs(Offset.Y) + FMath::Abs(Offset.Z);
			const int32 Distance = bMoore ? Chebyshev : Manhattan;
			if (Distance < 1 || Distance > Radius)
			{
				bMetricOk = false;
			}
		}
		TestTrue(FString::Printf(TEXT("%s: офсеты не повторяются"), Label), Unique.Num() == Offsets.Num());
		TestTrue(FString::Printf(TEXT("%s: все офсеты в пределах радиуса и без центра"), Label), bMetricOk);
	};
	CheckSet(VN2, 2, /*bMoore=*/false, TEXT("фон Нейман радиуса 2"));
	CheckSet(M2, 2, /*bMoore=*/true, TEXT("Moore радиуса 2"));

	// Поддержанность пар: радиус > 1 только у фон Неймана, и только до 2.
	TestTrue(TEXT("VN радиуса 1 поддержан"), IsNeighborhoodRadiusSupported(ENeighborhood::VonNeumann, 1));
	TestTrue(TEXT("Moore радиуса 1 поддержан"), IsNeighborhoodRadiusSupported(ENeighborhood::Moore, 1));
	TestTrue(TEXT("VN радиуса 2 поддержан"), IsNeighborhoodRadiusSupported(ENeighborhood::VonNeumann, 2));
	TestFalse(TEXT("Moore радиуса 2 не поддержан"), IsNeighborhoodRadiusSupported(ENeighborhood::Moore, 2));
	TestTrue(TEXT("форма радиуса 1 поддержана"), IsNeighborhoodRadiusSupported(ENeighborhood::Edges, 1));
	TestFalse(TEXT("форма радиуса 2 не поддержана"), IsNeighborhoodRadiusSupported(ENeighborhood::Edges, 2));
	TestFalse(TEXT("диагонали радиуса 2 не поддержаны"), IsNeighborhoodRadiusSupported(ENeighborhood::Corners, 2));
	TestFalse(TEXT("радиус 0 не поддержан"), IsNeighborhoodRadiusSupported(ENeighborhood::VonNeumann, 0));
	TestFalse(TEXT("радиус за потолком не поддержан"), IsNeighborhoodRadiusSupported(ENeighborhood::VonNeumann, MaxNeighborhoodRadius + 1));

	// Правило доносит радиус до GPU-гало - без этого пачка теряла бы
	// пограничные клетки молча.
	const FCellularAutomatonRule Rule({ 1 }, { 2 }, ENeighborhood::VonNeumann, 2, 2);
	TestEqual(TEXT("правило помнит радиус"), Rule.GetNeighborRadius(), 2);
	TestEqual(TEXT("правило построило офсеты по радиусу"), Rule.GetNeighborOffsets().Num(), 24);

	const FCellularAutomatonRule DefaultRule({ 1 }, { 2 }, ENeighborhood::Moore, 2);
	TestEqual(TEXT("радиус по умолчанию - 1"), DefaultRule.GetNeighborRadius(), 1);
	TestEqual(TEXT("правило без радиуса - прежние 26 офсетов"), DefaultRule.GetNeighborOffsets().Num(), 26);

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
		TestEqual(TEXT("радиус без цифры - 1"), Parsed.Radius, 1);
	}

	// Хвостовая цифра радиуса. Отдельным блоком, потому что проверяется не
	// только само число, но и что имя соседства при этом разобрано верно -
	// цифра отрезается с конца, а не отделяется разделителем.
	{
		RuleStringParser::FParsedRule Parsed;
		FString Error;
		TestTrue(TEXT("'0-6/1,3/2/VN2' разбирается"), RuleStringParser::ParseRuleString(TEXT("0-6/1,3/2/VN2"), Parsed, Error));
		TestEqual(TEXT("радиус 2"), Parsed.Radius, 2);
		TestTrue(TEXT("соседство при радиусе 2"), Parsed.Neighborhood == ENeighborhood::VonNeumann);

		TestTrue(TEXT("'0-6/1,3/2/VonNeumann' разбирается"), RuleStringParser::ParseRuleString(TEXT("0-6/1,3/2/VonNeumann"), Parsed, Error));
		TestEqual(TEXT("длинное имя без цифры - радиус 1"), Parsed.Radius, 1);
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
	};

	for (const FString& Rule : Rules)
	{
		RuleStringParser::FParsedRule First;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("'%s' разбирается"), *Rule), RuleStringParser::ParseRuleString(Rule, First, Error)))
		{
			continue;
		}

		const FString Formatted = RuleStringParser::FormatRuleString(First.SurvivalCounts, First.BirthCounts, First.States, First.Neighborhood, First.Radius);

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
		TestEqual(FString::Printf(TEXT("'%s': радиус после round-trip"), *Rule), Second.Radius, First.Radius);

		// Строки радиуса 1 обязаны печататься БАЙТ В БАЙТ как раньше: иначе
		// каждая уже написанная строка (и каждая запись RulePresets) начала бы
		// возвращаться из round-trip'а в другом виде.
		if (First.Radius == 1)
		{
			TestFalse(FString::Printf(TEXT("'%s': радиус 1 не печатает цифру"), *Rule), Formatted.EndsWith(TEXT("1")));
		}
	}

	// Сжатие обратно в диапазоны - не косметика: без него строка растёт с
	// каждым round-trip'ом.
	TestEqual(TEXT("подряд идущие сжимаются в диапазон"),
		RuleStringParser::FormatRuleString({ 1, 2, 3, 7 }, { 4 }, 2, ENeighborhood::Moore), FString(TEXT("1-3,7/4/2/M")));
	TestEqual(TEXT("повторы схлопываются"),
		RuleStringParser::FormatRuleString({ 5, 5, 5 }, { 4 }, 2, ENeighborhood::VonNeumann), FString(TEXT("5/4/2/VN")));

	// Радиус в печати: 1 - отсутствием цифры (в т.ч. когда его передали явно),
	// 2 - цифрой.
	TestEqual(TEXT("явный радиус 1 цифры не печатает"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::VonNeumann, 1), FString(TEXT("5/4/2/VN")));
	TestEqual(TEXT("радиус 2 печатается цифрой"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::VonNeumann, 2), FString(TEXT("5/4/2/VN2")));

	// Токены форм. Ни один не должен заканчиваться цифрой - иначе он разобрался
	// бы как имя плюс радиус (см. ParseNeighborhoodName()).
	TestEqual(TEXT("токен рёбер"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::Edges), FString(TEXT("5/4/2/E")));
	TestEqual(TEXT("токен диагоналей"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::Corners), FString(TEXT("5/4/2/C")));
	TestEqual(TEXT("токен граней+рёбер"),
		RuleStringParser::FormatRuleString({ 5 }, { 4 }, 2, ENeighborhood::FacesEdges), FString(TEXT("5/4/2/FE")));

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
		// Радиус вне [1, MaxNeighborhoodRadius]...
		TEXT("1/2/2/VN0"),
		TEXT("1/2/2/VN3"),
		// ...и радиус у Moore: 124 соседа не влезают ни в шейдерный массив,
		// ни в 32-битные маски правила, поэтому это отказ, а не тихое
		// приведение к радиусу 1 (см. IsNeighborhoodRadiusSupported()).
		TEXT("1/2/2/M2"),
		// ...и радиус у формы: она определена как подмножество куба 3x3x3,
		// поэтому радиуса у неё нет вовсе.
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
		int32 Radius = 1;
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
		{ TEXT("бинарное 6-16/12-13/2/VN2"), { 12, 13 }, { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, ENeighborhood::VonNeumann, 2, 2 },
		{ TEXT("Generations 6-16/12-13/5/VN2"), { 12, 13 }, { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, ENeighborhood::VonNeumann, 5, 2 },
		// Формы: набор офсетов перестаёт быть "шаром вокруг клетки", и обе
		// стратегии обязаны это одинаково пережить. Рёбра дополнительно
		// интересны тем, что решётка распадается на две независимые
		// подрешётки - расхождение проявилось бы как перетекание между ними.
		{ TEXT("бинарное 3-8/5-6/2/E"), { 5, 6 }, { 3, 4, 5, 6, 7, 8 }, ENeighborhood::Edges, 2 },
		{ TEXT("бинарное 5-11/8-9/2/FE"), { 8, 9 }, { 5, 6, 7, 8, 9, 10, 11 }, ENeighborhood::FacesEdges, 2 },
	};

	for (const FRuleCase& Case : Cases)
	{
		const FCellularAutomatonRule Rule(Case.Birth, Case.Survival, Case.Neighborhood, Case.States, Case.Radius);

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
		int32 Radius = 1;
	};

	const TArray<FRuleCase> Cases = {
		{ TEXT("бинарное 4/4/2/M"), { 4 }, { 4 }, 2 },
		{ TEXT("Generations 4/4/5/M"), { 4 }, { 4 }, 5 },
		// Радиус 2 именно здесь важнее всего: гало пачки равно радиус*поколений,
		// то есть при радиусе 2 оно вдвое больше, чем было. Ошибка в этом
		// множителе не роняет ничего - она молча теряет пограничные клетки, и
		// расхождение с пошаговым прогоном единственное, что её показывает.
		{ TEXT("бинарное 6-16/12-13/2/VN2"), { 12, 13 }, { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, 2, ENeighborhood::VonNeumann, 2 },
	};

	for (const FRuleCase& Case : Cases)
	{
		const FCellularAutomatonRule Rule(Case.Birth, Case.Survival, Case.Neighborhood, Case.States, Case.Radius);
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
		StateGenerators::AnalyzeNeighborCounts(Cells, ENeighborhood::Moore, /*NeighborRadius=*/1, /*MaxSampleExtent=*/20, Histogram);

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

	// Границы окна. При States > 2 угасающие клетки рисуются, но живыми не
	// считаются - "видимо" ЗАКОННО выше "всего", и масштаб по одному ряду
	// срезал бы второй.
	{
		TArray<FGenerationSample> History;
		int64 MinGeneration = -1;
		int64 MaxGeneration = -1;
		int32 MaxY = -1;

		TestFalse(TEXT("на пустой истории границ нет"),
			GenerationHistory::ComputeBounds(History, MinGeneration, MaxGeneration, MaxY));

		GenerationHistory::NoteRendered(History, 5, 100, 900, 512);
		TestTrue(TEXT("границы одного замера"),
			GenerationHistory::ComputeBounds(History, MinGeneration, MaxGeneration, MaxY));
		TestEqual(TEXT("вырожденное окно"), MinGeneration, MaxGeneration);
		TestEqual(TEXT("потолок берётся по обоим рядам"), MaxY, 900);

		GenerationHistory::Append(History, 9, 2000, 512);
		TestTrue(TEXT("границы двух замеров"),
			GenerationHistory::ComputeBounds(History, MinGeneration, MaxGeneration, MaxY));
		TestEqual(TEXT("левый край"), MinGeneration, (int64)5);
		TestEqual(TEXT("правый край"), MaxGeneration, (int64)9);
		TestEqual(TEXT("потолок поднялся до живых"), MaxY, 2000);
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
			0, 11, 100.0, /*bLogScale=*/false, Alive, Rendered);

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
			3, 3, 20.0, /*bLogScale=*/false, Alive, Rendered);

		TestEqual(TEXT("точки построены"), Alive.Num(), 2);
		TestTrue(TEXT("X конечен"), FMath::IsFinite(Alive[0].X) && FMath::IsFinite(Alive[1].X));
		TestTrue(TEXT("Y конечен"), FMath::IsFinite(Alive[0].Y) && FMath::IsFinite(Alive[1].Y));
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

#endif // WITH_DEV_AUTOMATION_TESTS
