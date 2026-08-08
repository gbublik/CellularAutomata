// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Grid/DenseCellGrid.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"
#include "Automata/Simulation/ComputeStrategy/GpuComputeStrategy.h"
#include "Automata/Sonification/AutomataSonifierComponent.h"
#include "Async/Async.h"


// Sets default values
AAutomataOrchestrator::AAutomataOrchestrator()
{
	// Тик нужен для непрерывной симуляции (Start()/Stop()), но не должен
	// крутиться, пока симуляция не запущена явно.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// Оба компонента для отрисовки клеток создаются всегда - переключение
	// CellMeshComponentType в рантайме (см. GetActiveCellsMeshComponent())
	// просто выбирает, какой из них получает AddInstances/ClearInstances, без
	// пересоздания компонентов (Live Coding не умеет безопасно хот-патчить
	// смену класса CreateDefaultSubobject-компонента на уже существующих в
	// уровне акторах - см. CLAUDE.md). Клетки чисто визуальные, коллизия не
	// нужна и только замедляет добавление инстансов при большом их количестве.
	CellsMeshHierarchical = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("CellsMeshHierarchical"));
	CellsMeshHierarchical->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RootComponent = CellsMeshHierarchical;

	CellsMeshFlat = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CellsMeshFlat"));
	CellsMeshFlat->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CellsMeshFlat->SetupAttachment(CellsMeshHierarchical);
}

// Called when the game starts or when spawned
void AAutomataOrchestrator::BeginPlay()
{
	Super::BeginPlay();

	// PIE дублирует актор из редакторского мира вместе с текущим СОСТОЯНИЕМ
	// компонентов - если пользователь нажал GenerateRandom()/Next() прямо в
	// редакторе перед запуском игры, реальные инстансы на компонентах
	// дублируются в PIE-копию. Штатный путь (RenderGridImmediate()) чистит
	// не всё (см. doc-comment ClearAllCellInstances()) - чистим здесь явно
	// и безусловно, до того как что-либо ещё успеет вызвать рендер.
	ClearAllCellInstances();
	ClearBakedMesh();
	ClearGhostShape();

	InitializePlayerController();
	EnsureCellsRenderer();
	EnsureSelectionMeshComponent();

	// Тумблер формы приводится к фактическим полям решётки ДО первой генерации:
	// актор расставлен в уровне, а форма в нём не сохраняется (она следствие
	// четырёх полей, см. FCellShapePreset), так что без этой строки уровень с
	// ОЦК-настройками стартовал бы с подписью "Куб" и, что важнее, с мешем и
	// множителем масштаба от прошлой формы - то есть со щелями. Заодно это
	// подставляет меш из слота, если поле CellMesh осталось от времён, когда
	// слотов не было.
	SyncCellShapeFromLatticeFields();

	// Стартуем ТЕМ генератором, что выбран в GenerationParams (то же, что даёт
	// хоткей Y). Случайный шар - не отдельный путь, а обычное значение
	// EStateGeneratorType::RandomBall, поэтому и запасной ветки здесь больше
	// нет: падать было бы некуда и незачем.
	//
	// GenerateState() умеет ОТКАЗАТЬСЯ - по бюджету MaxGeneratedCells или на
	// ошибке генератора - и оставить сетку нетронутой. На старте это значит
	// пустой мир, но зато с внятным сообщением на экране (см. ShowStatusMessage()
	// внутри), а не с молча подменённой фигурой: если настройки генератора
	// заведомо не влезают, честнее показать это сразу, чем нарисовать что-то
	// другое и оставить пользователя гадать, почему.
	GenerateState();

	// Раньше вызывалось из PostActorCreated(), который срабатывает при любом
	// создании актора - в т.ч. просто при расстановке в редакторе вне PIE, не
	// только в игре. HUD-виджет никогда не должен всплывать вне реальной
	// игровой сессии - BeginPlay() гарантированно только PIE/игра.
	InitializeHUD();

	// По той же причине, что и HUD: звук не должен появляться при простой
	// расстановке актора в редакторе вне PIE, поэтому здесь, а не в
	// OnConstruction(). Компонент тикает сам и на выключенный тик актора не
	// смотрит - см. doc-comment UAutomataSonifierComponent.
	EnsureSonifier();
}

// Called every frame
void AAutomataOrchestrator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// HUD-сводки (см. Ui/HudStats.h) обновляются каждый тик, ДО веток
	// bFastStepActive/!bSimulationRunning ниже (у обеих есть ранний return) -
	// HUD должен показывать FPS/занятость даже когда симуляция на паузе, не
	// только пока Play/автошаг активны.
	UpdateHudStats();

	// Съёмка сюда больше не заглядывает: у UAutomataPhotoComponent свой тик,
	// включённый ровно на время снимка. Раньше здесь стояла проверка её
	// готовности, а сама съёмка насильно включала тик актора и восстанавливала
	// его по формуле - при том, что снимают обычно на паузе, когда этому тику
	// работать не за чем.

	// Разлитый по кадрам рендер (см. bEnableChunkedRender) продолжается
	// независимо от bSimulationRunning - если игру остановили посреди
	// "разлива", он всё равно должен доехать до конца, а не застрять
	// наполовину отрисованным.
	if (bChunkedRenderInProgress)
	{
		AdvanceChunkedRender();
	}

	// Срез привязан к камере, значит при полёте его надо перестраивать. Здесь,
	// ДО всех ранних возвратов ниже: разглядывают структуру обычно на паузе,
	// когда ни bSimulationRunning, ни bFastStepActive не выставлены. Именно
	// ради этого SetViewSliceEnabled() и включает тик сам (см. там), иначе
	// актор на паузе не тикает вовсе и срез бы застыл.
	// Не трогаем сетку, пока её читает фоновый шаг или дорисовывает чанковый
	// разлив - те же гварды, что у всех прочих путей рендера.
	if (!bStepInProgress && !bChunkedRenderInProgress && ShouldRefreshViewSlice())
	{
		RefreshRenderCullVolume();
	}

	// Автошаг Shift+F (см. StartFastStep()) - взаимоисключающ с
	// bSimulationRunning (Start() отказывает, пока это активно, и наоборот),
	// поэтому безопасно делить TimeSinceLastStep с обычным Play.
	// Пока включён "ждать разлив" (см. bWaitForChunkedRenderToFinish), не
	// запускаем следующий шаг, пока предыдущий чанковый "разлив" ещё
	// рисуется - AdvanceChunkedRender() выше в этом же Tick() уже мог его
	// как раз завершить, так что проверка сразу актуальна для этого кадра.
	const bool bBlockedByChunkedRender = bWaitForChunkedRenderToFinish && bChunkedRenderInProgress;

	// Интервал между фоновыми заходами: не 1/Speed, а (поколений за заход)/Speed -
	// иначе, когда заход считает пачку из StepsPerRender поколений (см.
	// BatchGenerations в StepAsync()), реальная частота поколений выросла бы в
	// StepsPerRender раз, и Speed начал бы означать "заходов в секунду" вместо
	// "поколений в секунду". LastDispatchGenerations - то, что последний
	// StepAsync() решил считать за заход (1, пока пачки не включились), так что
	// первый заход прогона паcуется как раньше, а дальше интервал сходится к
	// фактическому размеру пачки.
	const float GenerationsPerDispatch = float(FMath::Max(1, LastDispatchGenerations));

	// Серия в быстром режиме идёт без пауз между шагами: Speed задаёт темп для
	// ПРОСМОТРА, а съёмке он только мешает - файлы получатся те же самые, но
	// ждать придётся во столько раз дольше. Следующий заход всё равно стартует
	// не раньше, чем закончится предыдущий (bStepInProgress).
	const bool bSeriesRush = bSeriesCaptureActive && SliceCaptureParams.bSeriesFastMode;

	if (bFastStepActive)
	{
		TimeSinceLastStep += DeltaTime;
		const float StepInterval = GenerationsPerDispatch / FMath::Max(Speed, KINDA_SMALL_NUMBER);

		if ((bSeriesRush || TimeSinceLastStep >= StepInterval) && !bStepInProgress && !bBlockedByChunkedRender)
		{
			TimeSinceLastStep = 0.0f;
			StepAsync();
		}

		return;
	}

	if (!bSimulationRunning)
	{
		return;
	}

	// Копим DeltaTime и шагаем симуляцию с интервалом 1/Speed секунд - а не
	// раз в кадр - чтобы Speed действительно означал "шагов в секунду"
	// независимо от FPS, и чтобы правки Speed в Details panel подхватывались
	// немедленно (интервал пересчитывается каждый раз, а не кэшируется).
	TimeSinceLastStep += DeltaTime;
	const float StepInterval = GenerationsPerDispatch / FMath::Max(Speed, KINDA_SMALL_NUMBER);

	// Шаг считается асинхронно (см. StepAsync()) - пока предыдущий фоновый
	// счёт не завершился (bStepInProgress), новый не запускаем (гонка на
	// Grid), просто ждём. Раньше здесь был while-цикл, "нагоняющий"
	// пропущенные шаги за один тик - для синхронного Next() это было
	// безопасно, но для асинхронного шага означало бы запуск нескольких
	// фоновых Step() поверх друг друга. По умолчанию (bWaitForChunkedRenderToFinish
	// == false) рендер (в т.ч. чанковый "разлив") не гейтит следующий шаг
	// вовсе - если он ещё не закончился, когда готово новое поколение,
	// ApplyStepResult() сам его прерывает и перезапускает с нуля на новом
	// состоянии (см. RenderCurrentGrid()/BeginRender()); если же включено,
	// bBlockedByChunkedRender (выше) держит следующий StepAsync() в ожидании,
	// пока разлив не дорисуется сам, и тогда ApplyStepResult() уже не застаёт
	// bChunkedRenderInProgress истинным. Оставшееся время не копится "про
	// запас" - реальная скорость сама упрётся в то, сколько Step() занимает
	// на этой сетке (плюс, в режиме ожидания, во сколько занимает разлив).
	if ((bSeriesRush || TimeSinceLastStep >= StepInterval) && !bStepInProgress && !bBlockedByChunkedRender)
	{
		TimeSinceLastStep = 0.0f;
		StepAsync();
	}
}

void AAutomataOrchestrator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Дожидаемся фонового шага (если он в полёте) ДО того, как актор начнёт
	// разрушаться - иначе StepAsync()'s CurrentGridPtr (сырой Grid.Get()) может
	// пережить сам Grid и фоновый ParallelFor разыменует уже освобождённую
	// память (см. PendingStepFuture в заголовке).
	if (PendingStepFuture.IsValid())
	{
		PendingStepFuture.Wait();
	}

	Super::EndPlay(EndPlayReason);
}

void AAutomataOrchestrator::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	EnsureCellsRenderer();
	EnsureSelectionMeshComponent();
}

#if WITH_EDITOR
void AAutomataOrchestrator::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellMeshComponentType))
	{
		EnsureCellsRenderer();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellShape))
	{
		// Тумблер формы. Движок к этому моменту уже записал новое значение в
		// поле, но форма - это ещё четыре поля и меш, поэтому применяем её
		// целиком тем же путём, что консоль и HUD. Отказ (гексагональная
		// призма) SetCellShape() откатит сам, вернув подпись к фактической
		// геометрии.
		SetCellShape(CellShape);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellMaterial)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, AgeColors)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, AgeColorMaxAge)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, DecayColors)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, ColorRampSpace)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, ColorRampCurve)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, SelectionColor))
	{
		// Цвет - чистая функция уже посчитанного состояния, ждать следующего
		// поколения незачем (а на паузе его и не будет). Перерисовываем
		// текущее состояние на месте: только если сетка есть и фоновый шаг
		// её сейчас не читает - RenderGridImmediate() иначе гонялся бы с ним
		// за Grid, ровно как и все прочие пути, трогающие сетку.
		if (Grid && !bStepInProgress)
		{
			RenderGridImmediate();
		}
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellBorderWidth))
	{
		// Намеренно НЕ перерисовываем: ширина канта живёт в uniform-буфере
		// материала, а не в per-instance данных, поэтому достаточно записать её
		// в динамический инстанс. Тянуть сюда RenderGridImmediate() было бы
		// прямым вредом - на миллионах клеток каждое движение ползунка в
		// Details panel стоило бы полного AddInstances().
		EnsureCellMaterialInstance();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellCullStartDistance)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, CellCullEndDistance))
	{
		// Без этого правка чисел в Details panel (в т.ч. во время PIE) не
		// применялась вплоть до следующего фактического рендера (шага
		// симуляции) или переключения хоткеем B (SetCellCullingEnabled()
		// зовёт ApplyCellCullDistances() сама, немедленно) - на паузе или
		// между шагами значение выглядело "зафиксированным" и не
		// реагирующим на CellCullEndDistance, хотя число менялось честно.
		// ApplyCellCullDistances() безопасно звать когда угодно - трогает
		// только CellsMeshHierarchical/CellsMeshFlat/
		// SelectionMeshComponent (все существуют с конструктора) и
		// GamePC/Grid только под null-проверками для диагностического лога.
		ApplyCellCullDistances();
	}
	else if (bAutoGenerateOnParamChange && IsGenerationProperty(PropertyChangedEvent))
	{
		// Interactive приходит на КАЖДОМ кадре протяжки ползунка, а генерация
		// стирает сетку и заново заливает инстансы: на миллионах клеток одна
		// протяжка радиуса от края до края означала бы сотню полных
		// перестроений и намертво повешенный редактор. Ждём отпускания
		// (ValueSet) - тогда за жест платим ровно один раз.
		if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
		{
			// Через публичный GenerateState(), а не своим путём: оценка против
			// MaxGeneratedCells, отказ при фоновом шаге и точка возврата для R
			// обязаны быть теми же, что у Y. Отказ здесь безвреден - он
			// оставляет текущее состояние целым и пишет причину на экран.
			GenerateState();
		}
	}
}

bool AAutomataOrchestrator::IsGenerationProperty(const FPropertyChangedEvent& PropertyChangedEvent)
{
	// GenerationParams - структура, и GetPropertyName() отдаёт имя поля ВНУТРИ
	// неё (Radius, Amount, Type...), а не саму структуру. Спрашиваем внешний
	// член: одна проверка покрывает все поля разом, включая те, которых в
	// FStateGeneratorParams ещё нет. Для обычного свойства MemberProperty
	// совпадает с самим свойством, так что Seed ловится тем же кодом.
	const FName MemberName = PropertyChangedEvent.MemberProperty
		? PropertyChangedEvent.MemberProperty->GetFName()
		: PropertyChangedEvent.GetPropertyName();

	// Seed здесь наравне с параметрами: он входит в ту же формулу и правится в
	// той же петле подбора. MaxGeneratedCells, наоборот, НЕ входит - это
	// предохранитель, и его подъём означает "разреши построить", а не "построй
	// прямо сейчас"; перестроение по нему запускало бы ровно ту генерацию,
	// которую предохранитель только что отказался пропускать.
	return MemberName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, GenerationParams)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AAutomataOrchestrator, Seed);
}
#endif

void AAutomataOrchestrator::InitializePlayerController()
{
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		GamePC = Cast<AGamePlayerController>(PC);
		if (GamePC)
		{
			GamePC->SetCameraControlEnabled(true);
			UE_LOG(LogTemp, Warning, TEXT("GamePlayerController setup complete"));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Wrong PlayerController class! Using: %s"), *PC->GetClass()->GetName());
		}
	}
}

TUniquePtr<FCellGrid> AAutomataOrchestrator::CreateGrid() const
{
	return MakeUnique<FDenseCellGrid>(BuildLatticeTransform(), ChunkSize, States > 2);
}

FLatticeTransform AAutomataOrchestrator::BuildLatticeTransform() const
{
	return FLatticeTransform::MakeOrthogonal(CellSize, LatticeZScale);
}

FCellularAutomatonRule AAutomataOrchestrator::BuildRule() const
{
	// ЕДИНСТВЕННОЕ место, где решается, каким набором соседей считать. Это
	// не стилистика: правило строится в трёх местах (Next(), StepAsync() и
	// гистограмма Ctrl+Y), и если ветвление размножить, Ctrl+Y начнёт мерить
	// одну окрестность, пока симуляция идёт по другой. Расхождение без всяких
	// симптомов, кроме "числа выглядят неправильно без причины".
	const TArray<FIntVector> LatticeOffsets = BuildLatticeNeighborOffsets(NeighborhoodShape);
	if (LatticeOffsets.Num() > 0)
	{
		return FCellularAutomatonRule(BirthCounts, SurvivalCounts, LatticeOffsets, States);
	}

	return FCellularAutomatonRule(BirthCounts, SurvivalCounts, Neighborhood, States);
}

TUniquePtr<FCellularAutomatonComputeStrategy> AAutomataOrchestrator::CreateComputeStrategy() const
{
	switch (ComputeMethod)
	{
	case EComputeMethod::Gpu:
		return MakeUnique<FGpuComputeStrategy>(GpuVolumeCellLimit);
	case EComputeMethod::Cpu:
	default:
		return MakeUnique<FCpuComputeStrategy>();
	}
}
