// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/RuleStringParser.h"
#include "Automata/Generation/StateGenerators.h"
#include "Async/Async.h"


void AAutomataOrchestrator::ApplyRuleString()
{
	// Целиком делегирует TryApplyRuleString() - единственный путь применения
	// правила строкой (кнопка в Details panel, поле в HUD и пресеты приходят
	// сюда же), чтобы семантика "что именно и в каком порядке присваивается"
	// жила ровно в одном месте.
	FString Error;
	TryApplyRuleString(RuleString, Error);
}

bool AAutomataOrchestrator::TryApplyRuleString(const FString& InRuleString, FString& OutError)
{
	RuleStringParser::FParsedRule Parsed;
	if (!RuleStringParser::ParseRuleString(InRuleString, Parsed, OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("TryApplyRuleString: не удалось разобрать '%s' - %s"), *InRuleString, *OutError);
		return false;
	}

	// Присваиваем по имени поля, не позиционно - порядок полей в строке
	// (Survival, затем Birth) не совпадает с порядком объявления
	// BirthCounts/SurvivalCounts здесь.
	SurvivalCounts = Parsed.SurvivalCounts;
	BirthCounts = Parsed.BirthCounts;
	States = Parsed.States;
	Neighborhood = Parsed.Neighborhood;

	// Строку, пришедшую параметром (из HUD или из пресета), кладём в
	// UPROPERTY - иначе Details panel показывал бы прежнее правило, хотя
	// считается уже по новому. При вызове из ApplyRuleString() это
	// самоприсваивание, безвредное.
	RuleString = InRuleString;

	OutError.Reset();

	UE_LOG(LogTemp, Log, TEXT("TryApplyRuleString: '%s' -> BirthCounts=%d знач., SurvivalCounts=%d знач., States=%d, Neighborhood=%s"),
		*InRuleString, BirthCounts.Num(), SurvivalCounts.Num(), States,
		GetNeighborhoodDisplayName(Neighborhood));

	// Ловушка, которая иначе не даёт вообще никакого симптома, кроме "правило
	// ведёт себя не как написано": BuildRule() предпочитает список смещений
	// формы, когда та его задаёт, поэтому на такой решётке токен соседства из
	// строки разобран, записан в поле и не применён - работают только
	// Birth/Survival/States.
	if (NeighborhoodShape != ELatticeNeighborhood::Shells)
	{
		UE_LOG(LogTemp, Warning, TEXT("TryApplyRuleString: активна форма клетки со своим списком соседей (%s) - окрестность '%s' из строки правила не применится"),
			*GetLatticeNeighborhoodDisplayName(NeighborhoodShape), GetNeighborhoodDisplayName(Neighborhood));
	}

	return true;
}

FString AAutomataOrchestrator::GetActiveRuleString() const
{
	return RuleStringParser::FormatRuleString(SurvivalCounts, BirthCounts, States, Neighborhood);
}

TArray<FRulePreset> AAutomataOrchestrator::GetRulePresets() const
{
	return RulePresets::GetAll();
}

void AAutomataOrchestrator::ApplyRulePreset(int32 PresetIndex, bool bApplySpawnSettings)
{
	const TArray<FRulePreset>& Presets = RulePresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyRulePreset: индекс %d вне диапазона (пресетов: %d)"), PresetIndex, Presets.Num());
		return;
	}

	const FRulePreset& Preset = Presets[PresetIndex];

	// Разбираем строку ЗАРАНЕЕ, ничего ещё не применяя: пресет, привязанный к
	// решётке, ниже пересоберёт сетку, и делать это ради правила, которое
	// потом не применится, значило бы стереть состояние ни за чем. Разбор
	// дешёвый, а второй раз он произойдёт уже внутри TryApplyRuleString() -
	// единственного пути применения правила, который мы обходить не хотим.
	RuleStringParser::FParsedRule Probe;
	FString Error;
	if (!RuleStringParser::ParseRuleString(Preset.RuleString, Probe, Error))
	{
		// Строки в таблице пресетов константны, так что сюда можно попасть
		// только если таблица разъехалась с парсером - это ошибка кода, а не
		// пользовательский ввод, поэтому Error, а не Warning.
		UE_LOG(LogTemp, Error, TEXT("ApplyRulePreset: пресет '%s' содержит неразбираемое правило '%s' - %s"),
			*Preset.Name, *Preset.RuleString, *Error);
		return;
	}

	// Решётка - ПЕРЕД правилом. Пресет формы выставляет в том числе соседство,
	// так что в обратном порядке он затёр бы токен, только что разобранный из
	// строки правила (у "Плоской жизни" это /PM - ровно то, ради чего привязка
	// и заведена). Трогаем только если она уже не та: смена решётки
	// пересобирает сетку, а пресет правила по умолчанию состояние не рушит.
	bool bLatticeChanged = false;
	if (Preset.bRequiresCellShape && CellShape != Preset.RequiredCellShape)
	{
		const int32 ShapeIndex = CellShapePresets::IndexOf(Preset.RequiredCellShape);
		bLatticeChanged = ShapeIndex != INDEX_NONE && ApplyCellShapeFields(ShapeIndex);
		if (!bLatticeChanged)
		{
			// Форма не применилась (её нет в таблице или она требует ещё не
			// реализованной решётки). Правило всё равно применим - оно само по
			// себе законно, - но сказать об этом надо: считаться оно будет не по
			// тому соседству, которое записано в его строке.
			UE_LOG(LogTemp, Warning, TEXT("ApplyRulePreset: пресет '%s' требует форму %d, применить её не удалось - правило пойдёт на текущей решётке"),
				*Preset.Name, static_cast<int32>(Preset.RequiredCellShape));
		}
	}

	if (!TryApplyRuleString(Preset.RuleString, Error))
	{
		// Недостижимо: строку только что разобрал тот же парсер. Оставлено
		// защитой от расхождения ParseRuleString() и TryApplyRuleString().
		UE_LOG(LogTemp, Error, TEXT("ApplyRulePreset: правило '%s' разобралось, но не применилось - %s"),
			*Preset.RuleString, *Error);
		return;
	}

	if (bApplySpawnSettings)
	{
		// Настройки спавна из пресета едут в параметры ГЕНЕРАТОРА, а не в
		// отдельный блок Automata|Random - того больше нет. Тип выставляется
		// явно: пресеты каталога описывают именно случайный шар заданной
		// плотности, и применить их радиус к, скажем, решётке из плит значило бы
		// молча сменить смысл числа.
		GenerationParams.Type = EStateGeneratorType::RandomBall;
		GenerationParams.Radius = Preset.SpawnRadius;
		GenerationParams.Amount = Preset.Amount;
	}

	UE_LOG(LogTemp, Log, TEXT("ApplyRulePreset: '%s' (%s)%s"),
		*Preset.Name, *Preset.RuleString,
		bApplySpawnSettings
			? *FString::Printf(TEXT(", Radius=%d, Amount=%d"), GenerationParams.Radius, GenerationParams.Amount)
			: TEXT(""));

	// Единственный случай, когда пресет правила рушит состояние: решётка
	// хранится внутри сетки, и клетки старой стоят по прежней геометрии.
	// Генерация одна на всё применение - поля формы и правило к этому моменту
	// уже на местах.
	if (bLatticeChanged)
	{
		ShowStatusMessage(StatusKey_CellShape, FString::Printf(
			TEXT("Пресет '%s' требует другую решётку - сетка построена заново"), *Preset.Name));
		GenerateState();
	}
}

TArray<FCellShapePreset> AAutomataOrchestrator::GetCellShapePresets() const
{
	return CellShapePresets::GetAll();
}

UStaticMesh* AAutomataOrchestrator::GetCellMeshForShape(ECellShape Shape) const
{
	// Единственное место, где имя формы превращается в поле-слот. Switch без
	// default: добавление шестой формы в перечисление (чего не будет -
	// классификация Фёдорова закрыта) обязано стать ошибкой компиляции, а не
	// молчаливым nullptr.
	switch (Shape)
	{
	case ECellShape::Cube:					return CubeMesh;
	case ECellShape::HexagonalPrism:		return HexagonalPrismMesh;
	case ECellShape::RhombicDodecahedron:	return RhombicDodecahedronMesh;
	case ECellShape::ElongatedDodecahedron:	return ElongatedDodecahedronMesh;
	case ECellShape::TruncatedOctahedron:	return TruncatedOctahedronMesh;
	}

	return nullptr;
}

void AAutomataOrchestrator::SetCellShape(ECellShape NewShape)
{
	const int32 Index = CellShapePresets::IndexOf(NewShape);
	if (Index == INDEX_NONE)
	{
		// Форма есть в перечислении, но её нет в таблице - расхождение двух
		// таблиц, то есть ошибка кода (её и стережёт тест
		// CellShape.PresetTableCoversEnum), а не пользовательский ввод.
		UE_LOG(LogTemp, Error, TEXT("SetCellShape: формы %d нет в таблице пресетов"), static_cast<int32>(NewShape));
	}
	else if (ApplyCellShapeFields(Index))
	{
		// Решётка поменялась - сетку надо построить заново: старая хранит
		// прежний шаг внутри себя, и клетки в ней стоят по прежней геометрии.
		GenerateState();
		return;
	}

	// Отказ. Поле CellShape к этому моменту уже могло быть переписано движком
	// (тумблер в Details panel пишет значение ДО PostEditChangeProperty), и
	// оставить его так значило бы показывать форму, которая не применена.
	// Восстанавливаем подпись по фактическим полям - тем же кодом, что после
	// загрузки файла.
	SyncCellShapeFromLatticeFields();
}

void AAutomataOrchestrator::SyncCellShapeFromLatticeFields()
{
	const TArray<FCellShapePreset>& Presets = CellShapePresets::GetAll();
	for (int32 Index = 0; Index < Presets.Num(); ++Index)
	{
		const FCellShapePreset& Preset = Presets[Index];

		// Формы, которые применить нельзя, и распознавать нечего: у
		// гексагональной призмы поля решётки совпадают с кубическими (скошенность
		// живёт не в них), так что без этой строки куб мог бы опознаться призмой.
		if (CellShapePresets::RequiresShearedLattice(Preset))
		{
			continue;
		}

		// Ключ - ровно те три поля, которыми формы различаются между собой:
		// какая подрешётка заселена, каким списком заданы соседи и как решётка
		// растянута. Множитель меша в ключ не входит намеренно - он следствие
		// формы, а не признак, и его-то мы здесь и восстанавливаем (в .casave
		// он не пишется вовсе, см. FAutomatonSaveHeader).
		if (Preset.ParityFilter != GenerationParams.ParityFilter
			|| Preset.NeighborhoodShape != NeighborhoodShape
			|| !FMath::IsNearlyEqual(Preset.LatticeZScale, LatticeZScale, 0.01f))
		{
			continue;
		}

		CellShape = Preset.Shape;
		ActiveCellShapePresetIndex = Index;
		CellMeshScaleMultiplier = Preset.CellMeshScaleMultiplier;
		if (UStaticMesh* ShapeMesh = GetCellMeshForShape(Preset.Shape))
		{
			CellMesh = ShapeMesh;
		}

		// Кант нормирован в каждом меше по-своему, поэтому его поправка меняется
		// вместе с формой (см. FCellShapePreset::BorderWidthScale). Значение
		// живёт в uniform-буфере материала, а не в per-instance данных, так что
		// перерисовывать ради него ничего не нужно.
		EnsureCellMaterialInstance();

		UE_LOG(LogTemp, Log, TEXT("SyncCellShapeFromLatticeFields: поля решётки опознаны как '%s'"), *Preset.Name);
		return;
	}

	// Комбинация, которой не соответствует ни одна из пяти форм, - законное
	// состояние: поля обычные и правятся руками (на этом стоит тест
	// Generation.ParityFilter - Moore на ГЦК обязан ломать чётность). Тумблер
	// в этом случае остаётся на прошлом значении и просто неактуален.
	UE_LOG(LogTemp, Log, TEXT("SyncCellShapeFromLatticeFields: комбинация полей не совпала ни с одной формой - тумблер оставлен как есть"));
}

void AAutomataOrchestrator::ApplyCellShapePreset(int32 PresetIndex)
{
	if (!ApplyCellShapeFields(PresetIndex))
	{
		return;
	}

	// Решётка поменялась - сетку надо построить заново: старая хранит прежний
	// шаг внутри себя, и клетки в ней стоят по прежней геометрии.
	GenerateState();
}

bool AAutomataOrchestrator::ApplyCellShapeFields(int32 PresetIndex)
{
	const TArray<FCellShapePreset>& Presets = CellShapePresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyCellShapePreset: индекс %d вне диапазона (форм: %d)"), PresetIndex, Presets.Num());
		return false;
	}

	const FCellShapePreset& Preset = Presets[PresetIndex];

	// Гексагональная призма требует скошенного отображения в мир, которого
	// сейчас нет. Отказываемся вслух, а не выставляем настройки, которые
	// нарисуют шестиугольники на кубической решётке - это выглядело бы
	// правдоподобно и было бы неверно, ровно та ошибка, из-за которой прошлая
	// попытка гекс-решётки была отменена. Никакой меш этого не чинит, поэтому
	// отказ стоит ДО подстановки слота.
	if (CellShapePresets::RequiresShearedLattice(Preset))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyCellShapePreset: '%s' требует скошенной решётки, которая ещё не реализована"), *Preset.Name);
		ShowStatusMessage(StatusKey_CellShape, FString::Printf(
			TEXT("Форма '%s' ещё не поддержана: нужна скошенная решётка"), *Preset.Name));
		return false;
	}

	// Пишем поля напрямую, а не через сеттеры: каждый из них перерисовывает
	// сетку, и четыре вызова стоили бы трёх лишних полных рендеров на
	// миллионах инстансов. Рендер один, в самом конце.
	GenerationParams.ParityFilter = Preset.ParityFilter;
	Neighborhood = Preset.Neighborhood;
	NeighborhoodShape = Preset.NeighborhoodShape;
	LatticeZScale = Preset.LatticeZScale;
	CellMeshScaleMultiplier = Preset.CellMeshScaleMultiplier;
	ActiveCellShapePresetIndex = PresetIndex;
	CellShape = Preset.Shape;

	// Меш - из слота этой формы. Пустой слот НЕ ошибка и не повод отказываться:
	// прежний меш даст щели или наложение, но решётка при этом честная, и
	// увидеть её так всё же лучше, чем не переключиться вовсе. Говорим вслух -
	// ровно как о разъехавшихся пропорциях ниже.
	FString MeshWarning;
	if (UStaticMesh* ShapeMesh = GetCellMeshForShape(Preset.Shape))
	{
		CellMesh = ShapeMesh;
	}
	else
	{
		MeshWarning = TEXT(" | слот меша пуст, оставлен прежний");
		UE_LOG(LogTemp, Warning, TEXT("ApplyCellShapePreset: слот меша формы '%s' пуст - назначьте ассет в Details panel (Automata|Cells|Meshes)"),
			*Preset.Name);
	}

	// Движок пропорции меша не проверяет никак: неверный ассет даёт щели или
	// наложение, а это неотличимо от неверно выбранной решётки. Поэтому
	// сверяем и говорим вслух - в статус-строку, а не только в лог (прецедент
	// - CellMaterial без ноды PerInstanceCustomData3Vector, который молча не
	// работает). Проверка стоит ПОСЛЕ подстановки слота, так что ловит она
	// именно тот меш, которым и будет рисоваться, - в том числе ассет,
	// положенный не в свой слот.
	if (CellMesh)
	{
		const FVector MeshAabb = CellMesh->GetBounds().BoxExtent * 2.0;
		if (!MeshAabb.IsNearlyZero())
		{
			// Сравниваем ПРОПОРЦИИ, а не абсолютный размер: рендерер всё равно
			// нормирует меш по его X-габариту, так что важно лишь отношение
			// сторон.
			const FVector Normalized = MeshAabb / MeshAabb.X;
			const FVector Expected = Preset.ExpectedMeshAabb / Preset.ExpectedMeshAabb.X;
			if (!Normalized.Equals(Expected, 0.01))
			{
				MeshWarning = FString::Printf(TEXT(" | меш имеет пропорции %.2f:%.2f:%.2f вместо %.2f:%.2f:%.2f"),
					Normalized.X, Normalized.Y, Normalized.Z, Expected.X, Expected.Y, Expected.Z);
				UE_LOG(LogTemp, Warning, TEXT("ApplyCellShapePreset: '%s' ожидает меш с габаритом %s, а назначенный имеет %s - будут щели или наложение"),
					*Preset.Name, *Preset.ExpectedMeshAabb.ToCompactString(), *MeshAabb.ToCompactString());
			}
		}
	}

	// Число соседей берётся тем же способом, что и симуляцией (BuildRule()), а
	// не пересчётом из пресета: если они разойдутся, увидеть это надо здесь, а
	// не по странной картинке.
	const int32 ActualNeighborCount = BuildNeighborOffsetsForAnalysis().Num();
	UE_LOG(LogTemp, Log, TEXT("ApplyCellShapePreset: '%s' - %d граней, соседей %d, чётность %d, Z x%.3f, меш x%.1f"),
		*Preset.Name, Preset.FaceCount, ActualNeighborCount, static_cast<int32>(Preset.ParityFilter),
		Preset.LatticeZScale, Preset.CellMeshScaleMultiplier);
	if (ActualNeighborCount != Preset.FaceCount)
	{
		// Одна грань на соседа - это определение ячейки Вороного, а не
		// пожелание. Расхождение значит, что узор растёт туда, где клетки
		// визуально не соприкасаются (или наоборот).
		UE_LOG(LogTemp, Warning, TEXT("ApplyCellShapePreset: у формы '%s' %d граней, а соседей %d - рост не совпадёт с видимыми контактами"),
			*Preset.Name, Preset.FaceCount, ActualNeighborCount);
	}

	// Поправка ширины канта - тоже свойство формы (см.
	// FCellShapePreset::BorderWidthScale): без этой строки кант остался бы от
	// прежней решётки до следующего движения ползунка.
	EnsureCellMaterialInstance();

	ShowStatusMessage(StatusKey_CellShape, FString::Printf(TEXT("Форма клетки: %s (%d граней)%s"),
		*Preset.Name, Preset.FaceCount, *MeshWarning));

	// Сетку здесь НЕ пересобираем - это делает вызывающий (см. doc-comment):
	// ApplyRulePreset() успевает между этой строкой и пересборкой применить
	// правило, чтобы заплатить за генерацию один раз, а не два.
	return true;
}

void AAutomataOrchestrator::SpawnRuleVerificationPattern()
{
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnRuleVerificationPattern: фоновый шаг StepAsync() ещё считается - подождите его завершения"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnRuleVerificationPattern: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!GetActiveCellsMeshComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnRuleVerificationPattern: активный CellsMesh-компонент отсутствует"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnRuleVerificationPattern: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	ClearBakedMesh();
	ClearGhostShape();

	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	ResetGenerationCounter();
	SelectedCells.Reset();

	// Все три паттерна - классические 2D-фигуры (см. doc-comment в заголовке),
	// целиком в плоскости Z=0, разнесены минимум на 6 пустых клеток друг от
	// друга (радиус влияния Moore - 1 клетка, этого с большим запасом
	// достаточно, чтобы они не мешали друг другу).
	TArray<FIntVector> Cells = {
		// Блок (неподвижка) - 2x2, должен остаться абсолютно неизменным на
		// любом числе шагов.
		{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},

		// Мигалка (осциллятор периода 2) - горизонтальная тройка, каждый шаг
		// переключается в вертикальную и обратно.
		{10, 0, 0}, {11, 0, 0}, {12, 0, 0},

		// Планер - классическая ориентация, за 4 поколения сдвигается на
		// (+1, +1), сохраняя форму.
		{21, 0, 0}, {22, 1, 0}, {20, 2, 0}, {21, 2, 0}, {22, 2, 0},
	};

	for (const FIntVector& Cell : Cells)
	{
		Grid->SetAlive(Cell, true);
	}

	// Та же "точка возврата", что и у GenerateRandom()/StartFromSelection() -
	// R воспроизводит ровно этот паттерн заново, не случайную генерацию.
	InitialStateCells = Cells;

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("SpawnRuleVerificationPattern: посажены блок/мигалка/планер (%d клеток) - выставьте BirthCounts=[3], SurvivalCounts=[2,3], Neighborhood=Moore и шагайте F, сверяя с классическим поведением 2D Game of Life"),
		Grid->Num());
}

TArray<FIntVector> AAutomataOrchestrator::BuildNeighborOffsetsForAnalysis() const
{
	// Ровно тот же выбор, что в BuildRule(), - гистограмма обязана мерить то
	// же соседство, по которому идёт симуляция.
	const TArray<FIntVector> LatticeOffsets = BuildLatticeNeighborOffsets(NeighborhoodShape);
	return LatticeOffsets.Num() > 0 ? LatticeOffsets : FCellularAutomatonRule::BuildNeighborOffsets(Neighborhood);
}

void AAutomataOrchestrator::AnalyzeLiveStructure()
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("AnalyzeLiveStructure: сетка ещё не создана"));
		return;
	}

	// Гвард на bStepInProgress намеренно НЕ ставится: фоновый шаг читает
	// *Grid, эта функция тоже только читает, а подменить Grid может лишь
	// ApplyStepResult() - то есть игровой поток, тот же, что выполняет эту
	// функцию. Двух одновременных читателей const-структуры достаточно.
	TArray<FIntVector> AliveCells;
	Grid->GetAliveCells(AliveCells);

	if (AliveCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("AnalyzeLiveStructure: живых клеток нет"));
		ShowStatusMessage(StatusKey_Generation, TEXT("Гистограмма: живых клеток нет"));
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();

	StateGenerators::FNeighborHistogram Histogram;
	StateGenerators::AnalyzeNeighborCounts(AliveCells, BuildNeighborOffsetsForAnalysis(), LiveAnalysisSampleExtent, Histogram);

	const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

	// Доля выборки печатается всегда: на эволюционировавшей структуре
	// центральный подкуб может оказаться и всей структурой, и одним процентом
	// от неё, а по самой гистограмме этого не видно.
	const double SampleShare = AliveCells.Num() > 0
		? 100.0 * double(Histogram.SampledAlive) / double(AliveCells.Num())
		: 0.0;

	// Правило строится из тех же Details-panel свойств, по которым идёт
	// симуляция (та же конвенция "пересобирать каждый вызов, ничего не
	// кэшировать", что в Next()/StepAsync()) - иначе сводка ниже могла бы
	// описывать не то правило, которое реально считает.
	const FCellularAutomatonRule Rule(BirthCounts, SurvivalCounts, Neighborhood, States);

	// Сводка - ради неё вся функция и нужна: гистограмма отвечает на вопрос
	// "какое распределение", а это - на вопрос "куда по нему бьют пороги".
	int64 Doomed = 0;
	for (int32 Count = 0; Count < Histogram.AliveByCount.Num(); ++Count)
	{
		if (!Rule.GetSurvivalCounts().Contains(Count))
		{
			Doomed += Histogram.AliveByCount[Count];
		}
	}

	int64 Births = 0;
	for (int32 Count = 0; Count < Histogram.EmptyByCount.Num(); ++Count)
	{
		if (Rule.GetBirthCounts().Contains(Count))
		{
			Births += Histogram.EmptyByCount[Count];
		}
	}

	const int64 Survivors = Histogram.SampledAlive - Doomed;
	const double DoomedShare = Histogram.SampledAlive > 0
		? 100.0 * double(Doomed) / double(Histogram.SampledAlive)
		: 0.0;
	// Оценка, а не точное число: рождения считаются по пустым клеткам,
	// примыкающим к выборке, а часть их лежит уже ЗА границей подкуба, тогда
	// как знаменатель - строго клетки выборки. На однородной структуре
	// отношение всё равно показывает направление и порядок величины.
	const double GrowthFactor = Histogram.SampledAlive > 0
		? double(Survivors + Births) / double(Histogram.SampledAlive)
		: 0.0;

	// При Generations клетка, переставшая выживать, не умирает, а уходит в
	// угасание - слово должно быть другим. Строка собирается заранее:
	// format-строка у Printf проверяется на этапе компиляции и обязана быть
	// литералом, тернарник прямо в вызове не соберётся.
	FString DoomedWord = TEXT("умрут");
	FString DecayNote;
	if (Rule.HasDecayStates())
	{
		DoomedWord = TEXT("уйдут в угасание");
		// Угасающие клетки не живые, поэтому в гистограмме они попадают в
		// "примыкающие пустые" - а родиться там нельзя (birth-immunity), так
		// что оценка рождений при Generations завышена.
		DecayNote = TEXT(" (Generations: рождения завышены - часть 'пустых' на деле угасающие и рождению не подлежат)");
	}

	UE_LOG(LogTemp, Log, TEXT("AnalyzeLiveStructure: правило %s, окрестность %s (%d соседей), поколение %d"),
		*GetActiveRuleString(), GetNeighborhoodDisplayName(Neighborhood),
		Rule.GetNeighborOffsets().Num(), GenerationCount);
	UE_LOG(LogTemp, Log, TEXT("AnalyzeLiveStructure: живых всего %d, в выборке %lld (%.1f%%, подкуб +-%d), посчитано за %.2f мс"),
		AliveCells.Num(), Histogram.SampledAlive, SampleShare, LiveAnalysisSampleExtent, ElapsedSeconds * 1000.0);
	UE_LOG(LogTemp, Log, TEXT("AnalyzeLiveStructure: %s"), *StateGenerators::DescribeHistogram(Histogram));
	UE_LOG(LogTemp, Log, TEXT("AnalyzeLiveStructure: ИТОГО %s %lld (%.1f%%), выживут %lld, родятся %lld -> нетто %+lld, x%.2f%s"),
		*DoomedWord, Doomed, DoomedShare, Survivors, Births, Births - Doomed, GrowthFactor, *DecayNote);

	ShowStatusMessage(StatusKey_Generation,
		FString::Printf(TEXT("Соседи: %s %lld из %lld (%.0f%%), родятся %lld, x%.2f - подробности в логе"),
			*DoomedWord, Doomed, Histogram.SampledAlive, DoomedShare, Births, GrowthFactor));
}
