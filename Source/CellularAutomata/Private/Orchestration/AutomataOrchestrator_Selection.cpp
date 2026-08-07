// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "Automata/Rendering/FilteredCellGridView.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Automata/Selection/CellSelection.h"
#include "Automata/Sonification/AutomataSonifierComponent.h"


void AAutomataOrchestrator::EnsureSelectionMeshComponent()
{
	if (SelectionMeshComponent && !SelectionRenderer)
	{
		// Пережил реинстансинг Live Coding (UPROPERTY), а SelectionRenderer
		// (обычный член) - нет; EnsureCellsRenderer() ловит ровно тот же
		// сценарий для CellsRenderer.
		SelectionRenderer = MakeUnique<FInstancedMeshCellGridRenderer>(SelectionMeshComponent);
	}

	if (SelectionMeshComponent)
	{
		return;
	}

	// Всегда обычный ISM, независимо от CellMeshComponentType - выделение
	// всегда маленькое подмножество, LOD-дерево кластеров HISM тут не даёт
	// выигрыша (см. doc-comment SelectionMeshComponent в заголовке).
	SelectionMeshComponent = NewObject<UInstancedStaticMeshComponent>(this);
	SelectionMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SelectionMeshComponent->SetupAttachment(CellsMeshHierarchical);
	SelectionMeshComponent->RegisterComponent();

	SelectionRenderer = MakeUnique<FInstancedMeshCellGridRenderer>(SelectionMeshComponent);
}

void AAutomataOrchestrator::RenderSelectionOverlay()
{
	EnsureSelectionMeshComponent();

	if (SelectedCells.Num() > 0 && !CellMaterial)
	{
		// Иначе подсветка молча не рисуется, и выглядит это как "выделение
		// не работает" - уже кусало при настройке.
		UE_LOG(LogTemp, Warning, TEXT("RenderSelectionOverlay: CellMaterial не назначен - подсветка выделения не будет видна, назначьте материал клеток в Details panel"));
	}

	if (!Grid || SelectedCells.Num() == 0 || !CellMaterial)
	{
		SelectionMeshComponent->ClearInstances();
		return;
	}

	// Отфильтровываем до реально живых - на случай, если SelectedCells
	// вызвали до какого-то не прошедшего через инвалидацию изменения Grid
	// (сегодня такого пути нет, но проверка дешёвая, а рассинхрон иначе тихий).
	const FColor HighlightColor = SelectionColor.ToFColor(/*bSRGB=*/false);
	TArray<FCellRenderInstance> SelectionInstances;
	SelectionInstances.Reserve(SelectedCells.Num());
	for (const FIntVector& Cell : SelectedCells)
	{
		if (Grid->IsAlive(Cell))
		{
			SelectionInstances.Add({ FVector3f(Grid->GridToWorld(Cell)), HighlightColor });
		}
	}

	if (SelectionInstances.Num() == 0)
	{
		SelectionMeshComponent->ClearInstances();
		return;
	}

	// Тот же материал, что и у обычных клеток - отличается только цветом в
	// per-instance custom data (см. doc-comment SelectionColor).
	SelectionRenderer->SetMesh(CellMesh);
	// Тот же динамический инстанс, что и у обычных клеток: подсветка отличается
	// только цветом из custom data, и кант на ней должен быть такой же ширины.
	SelectionRenderer->SetMaterial(EnsureCellMaterialInstance());
	// Чуть крупнее обычной клетки - иначе поверхности совпадают и мерцают
	// (z-fighting), см. doc-comment SelectionScaleMultiplier.
	//
	// Множитель клетки обязателен множителем, а не заменой: SelectionScaleMultiplier
	// задан ОТНОСИТЕЛЬНО клетки ("на 10% крупнее"), а не абсолютно. Пока
	// CellMeshScaleMultiplier был единицей, разницы не было; с ячейками решёток,
	// которым нужен масштаб 2 (ромбододекаэдр, усечённый октаэдр), подсветка
	// рисовалась вдвое МЕНЬШЕ клетки и целиком пряталась внутри неё - выделение
	// при этом работало, просто его не было видно.
	SelectionRenderer->SetScaleMultiplier(CellMeshScaleMultiplier * SelectionScaleMultiplier);

	// Всегда одним снимком - выделение всегда маленькое, чанкинг не нужен
	// даже во время непрерывного Play.
	SelectionRenderer->Render(*Grid, MoveTemp(SelectionInstances));
}

void AAutomataOrchestrator::SelectCellsInScreenRect(const FMatrix& ViewProjectionMatrix, const FVector2D& ViewportSize, const FVector2D& RectMin, const FVector2D& RectMax, ESelectionCombineMode CombineMode)
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellsInScreenRect: сетка не инициализирована"));
		return;
	}

	// Куб отсечения (см. bEnableRenderCullVolume) прячет клетки снаружи себя
	// от рендера (BuildCellRenderData() ограничивает по тем же границам) -
	// выделение обязано ловить ровно то же подмножество, иначе марки видят
	// клетки, которых физически нет на экране.
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	TArray<FIntVector> RectCells;
	if (CullVolume)
	{
		TArray<FIntVector> VisibleCells;
		Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), VisibleCells);
		FFilteredCellGridView VisibleView(*Grid, MoveTemp(VisibleCells));
		RectCells = CellSelection::SelectCellsInScreenRect(VisibleView, ViewProjectionMatrix, ViewportSize, RectMin, RectMax);
	}
	else
	{
		RectCells = CellSelection::SelectCellsInScreenRect(*Grid, ViewProjectionMatrix, ViewportSize, RectMin, RectMax);
	}
	CombineWithSelection(MoveTemp(RectCells), CombineMode);

	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("SelectCellsInScreenRect: выделено %d клеток (режим: %s)"),
		SelectedCells.Num(), *UEnum::GetValueAsString(CombineMode));
}

void AAutomataOrchestrator::SelectCellUnderCursor(const FVector& RayOrigin, const FVector& RayDirection, ESelectionCombineMode CombineMode)
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellUnderCursor: сетка не инициализирована"));
		return;
	}

	// Лимит обхода DDA: до дальнего края описанной сферы живых клеток -
	// дальше живых клеток гарантированно нет, шагать бессмысленно. Считаем
	// от ПОЛНОГО набора живых клеток (не от отфильтрованного по кубу ниже) -
	// это только верхняя граница длины луча, а не источник кандидатов, так
	// что запас безопасен и в режиме с активным кубом.
	FVector BoundsCenter = FVector::ZeroVector;
	float BoundsRadius = 0.0f;
	if (!ComputeAliveCellsBounds(BoundsCenter, BoundsRadius))
	{
		UE_LOG(LogTemp, Log, TEXT("SelectCellUnderCursor: живых клеток нет - выделять нечего"));
		return;
	}
	// Запас - НАИБОЛЬШИЙ габарит клетки: на решётке, растянутой по оси,
	// занижение до шага в плоскости давало бы недолёт луча вдоль вытянутой
	// оси, то есть промах по последней клетке.
	const double MaxDistance = FVector::Distance(RayOrigin, BoundsCenter) + BoundsRadius + Grid->GetLattice().GetMaxCellWorldExtent();

	// Тот же принцип, что у SelectCellsInScreenRect() выше - если куб
	// активен, клик должен "видеть" ровно то подмножество клеток, которое
	// реально нарисовано, а не всю сетку насквозь. FFilteredCellGridView::
	// IsAlive() (единственное, что использует DDA-обход PickCellAlongRay())
	// согласован с отфильтрованным набором - см. её doc-comment.
	ARenderCullVolume* CullVolume = GetActiveCullVolume();
	TUniquePtr<FFilteredCellGridView> VisibleView;
	const FCellGrid* PickGrid = Grid.Get();
	if (CullVolume)
	{
		TArray<FIntVector> VisibleCells;
		Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), VisibleCells);
		VisibleView = MakeUnique<FFilteredCellGridView>(*Grid, MoveTemp(VisibleCells));
		PickGrid = VisibleView.Get();
	}

	TArray<FIntVector> PickedCells;
	FIntVector PickedCell;
	if (CellSelection::PickCellAlongRay(*PickGrid, RayOrigin, RayDirection, MaxDistance, PickedCell))
	{
		PickedCells.Add(PickedCell);

		// Звук клетки. Мировая позиция - через ту же решётку, которой луч и
		// шёл, поэтому отдельной системы координат тут не заводится. Возраст
		// нормируется на AgeColorMaxAge - ТЕМ ЖЕ числом, которым клетка
		// красится, так что высота ноты совпадает с цветом: красная звучит
		// выше синей. Это не совпадение, а один и тот же параметр.
		if (bEnableSonification)
		{
			if (UAutomataSonifierComponent* Component = EnsureSonifier())
			{
				Component->PlayCellClick(Grid->GetLattice().GridToWorld(PickedCell),
					Grid->GetAge(PickedCell), AgeColorMaxAge);
			}
		}
	}

	// Пустой PickedCells (клик в пустоту) - тоже валидный ввод: Replace
	// очистит выделение (стандартное "кликнул мимо - снял выделение"),
	// Add/Subtract ничего не изменят.
	CombineWithSelection(MoveTemp(PickedCells), CombineMode);

	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("SelectCellUnderCursor: выделено %d клеток (режим: %s)"),
		SelectedCells.Num(), *UEnum::GetValueAsString(CombineMode));
}

void AAutomataOrchestrator::SelectCellsInCullVolume(ESelectionCombineMode CombineMode)
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellsInCullVolume: сетка не инициализирована"));
		return;
	}

	// EnsureRenderCullVolume() напрямую, не через bEnableRenderCullVolume -
	// куб как пространственная область существует независимо от того,
	// используется ли он сейчас для отсечения рендера (см. doc-comment в
	// заголовке).
	ARenderCullVolume* CullVolume = EnsureRenderCullVolume();
	if (!CullVolume)
	{
		UE_LOG(LogTemp, Warning, TEXT("SelectCellsInCullVolume: на уровне нет ARenderCullVolume - разместите его сначала"));
		return;
	}

	TArray<FIntVector> CellsInVolume;
	Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds(), CellsInVolume);
	CombineWithSelection(MoveTemp(CellsInVolume), CombineMode);

	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("SelectCellsInCullVolume: выделено %d клеток (режим: %s)"),
		SelectedCells.Num(), *UEnum::GetValueAsString(CombineMode));
}

void AAutomataOrchestrator::CombineWithSelection(TArray<FIntVector>&& NewCells, ESelectionCombineMode CombineMode)
{
	switch (CombineMode)
	{
	case ESelectionCombineMode::Add:
	{
		// Объединение без дублей: TSet по уже выделенным даёт O(1) проверку
		// на каждую новую клетку - выделения могут быть миллионными,
		// квадратичный Contains по TArray здесь недопустим.
		TSet<FIntVector> ExistingCells(SelectedCells);
		for (const FIntVector& Cell : NewCells)
		{
			if (!ExistingCells.Contains(Cell))
			{
				SelectedCells.Add(Cell);
			}
		}
		break;
	}
	case ESelectionCombineMode::Subtract:
	{
		const TSet<FIntVector> CellsToRemove(NewCells);
		SelectedCells.RemoveAll([&CellsToRemove](const FIntVector& Cell)
		{
			return CellsToRemove.Contains(Cell);
		});
		break;
	}
	case ESelectionCombineMode::Replace:
	default:
		SelectedCells = MoveTemp(NewCells);
		break;
	}
}

void AAutomataOrchestrator::ClearSelection()
{
	// Драг закрываем ПЕРВЫМ и через штатный путь: он вернёт подсветку на место
	// и, если сдвиг был, внесёт перенос в сетку. Бросить драг на полпути значило
	// бы потерять уже показанное пользователю перемещение.
	if (IsSelectionDragging())
	{
		EndSelectionDrag();
	}

	if (SelectedCells.Num() == 0)
	{
		return;
	}

	SelectedCells.Reset();
	RenderSelectionOverlay();
}

void AAutomataOrchestrator::InvertSelection()
{
	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("InvertSelection: сетка не инициализирована"));
		return;
	}

	// TSet по текущему выделению - O(1) проверка на каждую живую клетку,
	// та же причина, что и в Add/Subtract-ветках SelectCellsInScreenRect().
	const TSet<FIntVector> CurrentlySelected(SelectedCells);

	TArray<FIntVector> AliveCells;
	Grid->GetAliveCells(AliveCells);

	TArray<FIntVector> Inverted;
	Inverted.Reserve(FMath::Max(0, AliveCells.Num() - SelectedCells.Num()));
	for (const FIntVector& Cell : AliveCells)
	{
		if (!CurrentlySelected.Contains(Cell))
		{
			Inverted.Add(Cell);
		}
	}

	SelectedCells = MoveTemp(Inverted);
	RenderSelectionOverlay();

	UE_LOG(LogTemp, Log, TEXT("InvertSelection: выделено %d клеток (из %d живых)"), SelectedCells.Num(), AliveCells.Num());
}

void AAutomataOrchestrator::StartFromSelection()
{
	// Фоновый шаг (Next()/StepAsync()) в этот момент читает *Grid - замена
	// сетки у него под ногами разыменует освобождённую память. Тот же guard,
	// что и в Next()/GenerateRandom()/ResetToInitialState().
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: фоновый шаг ещё считается - подождите его завершения"));
		return;
	}

	if (SelectedCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: нет выделенных клеток - сначала выделите что-нибудь мышкой в режиме выделения (Tab)"));
		return;
	}

	if (!Grid)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: сетка не инициализирована"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartFromSelection: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	// Мировые координаты НЕ переносятся к началу координат - клетки остаются
	// там же, где их выделили (правила автомата трансляционно инвариантны, а
	// камера и так уже смотрит именно туда - см. doc-comment в заголовке).
	TUniquePtr<FCellGrid> NewGrid = CreateGrid();
	// Заодно строим InitialStateCells - точно тот же набор, что реально
	// попал в NewGrid (только реально живые из SelectedCells), а не сырой
	// SelectedCells, который в принципе мог содержать неактуальные записи -
	// это и есть "точка возврата" для последующего ResetToInitialState() (R).
	InitialStateCells.Reset();
	InitialStateCells.Reserve(SelectedCells.Num());
	for (const FIntVector& Cell : SelectedCells)
	{
		if (Grid->IsAlive(Cell))
		{
			NewGrid->SetAlive(Cell, true);
			NewGrid->SetAge(Cell, 0); // свежий старт, как только что рождённая клетка
			InitialStateCells.Add(Cell);
		}
	}

	Grid = MoveTemp(NewGrid);
	SelectedCells.Reset();
	StepsSinceLastRender = 0;
	ResetGenerationCounter();
	// Новый прогон убирает запечённый меш-снимок, если он был (см.
	// BakeCellsToMesh()) - как и в GenerateRandom()/ResetToInitialState().
	ClearBakedMesh();
	ClearGhostShape();

	if (GamePC)
	{
		GamePC->SetSelectionModeActive(false);
	}

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("StartFromSelection: новое состояние из %d клеток (запомнено как точка возврата для R)"), InitialStateCells.Num());
}

bool AAutomataOrchestrator::ComputeSelectedCellsBounds(FVector& OutCenter, float& OutRadius) const
{
	if (!Grid || SelectedCells.Num() == 0)
	{
		return false;
	}

	// Только ещё живые - та же защитная фильтрация, что в
	// RenderSelectionOverlay()/StartFromSelection(): выделение переживает шаги
	// симуляции, и клетка под ним могла давно умереть.
	TArray<FIntVector> Cells;
	Cells.Reserve(SelectedCells.Num());
	for (const FIntVector& Cell : SelectedCells)
	{
		if (Grid->IsAlive(Cell))
		{
			Cells.Add(Cell);
		}
	}

	return ComputeCellsBounds(Cells, OutCenter, OutRadius);
}
