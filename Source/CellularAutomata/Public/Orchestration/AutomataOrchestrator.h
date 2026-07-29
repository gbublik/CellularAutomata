// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Blueprint/UserWidget.h"
#include "CellularAutomata/Public/Core/PlayerController/GamePlayerController.h"
#include "CellularAutomata/Public/Ui/UiController.h"
#include "Automata/Grid/CellGrid.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "Automata/Rendering/ChunkedRenderOrder.h"
#include "Automata/Persistence/AutomatonSaveHeader.h"
#include "Automata/Selection/SelectionCombineMode.h"
#include "Automata/Simulation/Neighborhood.h"
#include "Automata/Simulation/RulePresets.h"
#include "GameFramework/PlayerController.h"
#include "AutomataOrchestrator.generated.h"

class UInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UProceduralMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class FCellularAutomatonComputeStrategy;
class ARenderCullVolume;

/** Метод расчёта шага симуляции. Обе реализации настоящие: Cpu - параллельный
 *  алгоритм с bucket-partitioned дедупом кандидатов, Gpu - RDG compute-шейдер
 *  (см. FGpuComputeStrategy; на CPU он откатывается только точечно, когда
 *  объём AABB не влезает в GpuVolumeCellLimit, с предупреждением в лог).
 *  Только Gpu умеет считать пачку поколений за один круг - см.
 *  FCellularAutomatonComputeStrategy::StepBatch(). */
UENUM(BlueprintType)
enum class EComputeMethod : uint8
{
	Cpu,
	Gpu
};

/** Реализация инстансированного компонента для отрисовки клеток. */
UENUM(BlueprintType)
enum class ECellMeshComponentType : uint8
{
	/** Обычный UInstancedStaticMeshComponent - без LOD-дерева кластеров,
	 *  дешевле на полную перестройку (ClearInstances+AddInstances каждый шаг). */
	Instanced,
	/** UHierarchicalInstancedStaticMeshComponent - строит LOD-дерево
	 *  кластеров инстансов (occlusion/distance culling по кластерам) -
	 *  выгоднее при больших количествах клеток, но каждая полная
	 *  перестройка дороже, чем у Instanced. */
	HierarchicalInstanced
};

/** Метрики последнего BuildCellRenderData() (см. doc-comment внутри неё).
 *  Два разных вида числа с разным смыслом: RenderedCellCount/TotalCellCount -
 *  ПАРА (клеток отрисовано/живых всего в сетке, после отсечения
 *  ARenderCullVolume вс. без него) - показывает масштаб расчётов, сколько
 *  из всей симуляции реально видно на экране. ВНИМАНИЕ: при правилах
 *  Generations (States > 2) RenderedCellCount может ЗАКОННО превышать
 *  TotalCellCount - угасающие клетки рисуются, но живыми не считаются, а
 *  Grid->Num() считает только живых. EstimatedUploadMB - ОДНО
 *  общее число, не пара - это оценка размера данных, которые реально
 *  уходят в AddInstances() (т.е. посчитана от RenderedCellCount, не от
 *  TotalCellCount) - как размер файла: единая величина, а не "до/после".
 *  Считается один раз и хранится здесь, а не пересчитывается заново на
 *  каждого потребителя - читают её и UE_LOG в BuildCellRenderData(), и HUD
 *  (UMainHudWidget) через GetLastRenderStats(), без дублирования подсчёта
 *  и риска разъехаться в цифрах между логом и экраном - тот же идиом, что
 *  FRenderTimings у FInstancedMeshCellGridRenderer. USTRUCT(BlueprintType) -
 *  т.к. читает Blueprint-виджет, не только нативный код (см. FHudStats
 *  ниже за тем же решением). */
USTRUCT(BlueprintType)
struct FCellRenderStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	int32 RenderedCellCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	int32 TotalCellCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	double EstimatedUploadMB = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	int32 BytesPerInstance = 0;
};

/** Сводка для HUD (см. AAutomataOrchestrator::GetHudStats()) - USTRUCT(BlueprintType)
 *  с BlueprintReadOnly-полями, читает её UMG/Blueprint-виджет (UMainHudWidget).
 *  Считается один раз за тик/шаг и хранится на оркестраторе - виджет её
 *  просто читает через GetHudStats(), ничего сам не пересчитывает. */
USTRUCT(BlueprintType)
struct FHudStats
{
	GENERATED_BODY()

	/** Фоновый StepAsync()/Next() сейчас считает поколение - см. bStepInProgress. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bIsComputing = false;

	/** Чанковый рендер сейчас "разливается" по кадрам - см. bChunkedRenderInProgress. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bIsRendering = false;

	/** Сглаженный FPS движка (GAverageFPS) - не считаем сами, берём готовое. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	float CurrentFPS = 0.0f;

	/** Сколько поколений посчитано с последнего GenerateRandom()/
	 *  ResetToInitialState() - см. AAutomataOrchestrator::GenerationCount. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int64 GenerationCount = 0;

	/** Скользящая частота поколений в секунду - обновляется раз в секунду,
	 *  не каждый кадр (см. UpdateGenerationsPerSecond()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	float GenerationsPerSecond = 0.0f;

	/** Простая оценка объёма данных, загружаемых в GPU-буфер для
	 *  compute-шейдера на последнем шаге (FGpuComputeStrategy::Step()'s
	 *  битовый входной буфер) - 0, если последний шаг считался на CPU
	 *  (там нет такой загрузки вовсе) или сетка ещё не запускалась.
	 *  Специально НЕ пересчитывается отдельным сканированием сетки -
	 *  FGpuComputeStrategy и так строит этот буфер каждый GPU-шаг, здесь
	 *  просто читается уже посчитанное им число (см.
	 *  FCellularAutomatonComputeStrategy::GetLastComputeUploadBytes()),
	 *  без лишней нагрузки на многомиллионных сетках. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	double EstimatedGpuComputeUploadMB = 0.0;

	/** Сколько клеток сейчас живо во всей сетке - Grid->Num(), О(1) (счётчик
	 *  ведут сами чанки, полного скана нет). Отличается от
	 *  FCellRenderStats::TotalCellCount тем, что обновляется каждый тик, а не
	 *  только на рендере: после Delete/выделения число меняется сразу, ещё до
	 *  следующего поколения. 0, если сетка не создана (в т.ч. после
	 *  BakeCellsToMesh(), который её освобождает). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 AliveCellCount = 0;

	/** Сколько клеток сейчас в выделении (см. SelectedCells) - HUD'у, чтобы
	 *  показывать, есть ли что извлекать/удалять, и сколько именно. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 SelectedCellCount = 0;

	/** Заданная скорость симуляции - AAutomataOrchestrator::Speed, т.е.
	 *  ЦЕЛЕВОЕ число поколений в секунду (крутится +/-). Отличается от
	 *  GenerationsPerSecond выше, которое измеряет ФАКТИЧЕСКУЮ частоту: на
	 *  больших сетках одно поколение считается дольше 1/Speed, и фактическая
	 *  оказывается кратно ниже заданной. Оба поля нужны именно в паре - по
	 *  расхождению между ними и видно, что симуляция упёрлась в вычисления, а
	 *  не в настройку. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	float SimulationSpeed = 0.0f;

	/** Сколько поколений считается на один рендер - AAutomataOrchestrator::
	 *  StepsPerRender (крутится T/G). 1 - рисуется каждое поколение. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 StepsPerRender = 1;

	/** Текущая максимальная скорость полёта камеры, юнитов в секунду -
	 *  читается прямо с UFloatingPawnMovement::MaxSpeed управляемого пешки, а
	 *  не считается из CameraSpeedMultiplier: пока держат Left Shift,
	 *  контроллер уже умножил MaxSpeed (см. AGamePlayerController::
	 *  OnSpeedBoostStarted()), и HUD должен показывать фактическую скорость
	 *  вместе с ускорением, а не базовую. 0, если пешка/её движение ещё не
	 *  готовы (в т.ч. вне PIE). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	float CameraSpeed = 0.0f;

	// --- Режимы работы: зеркала живых переключателей, по одному на хоткей ---
	// Все они зеркала, а не отдельное состояние: HUD показывает ровно то, что
	// переключают хоткеи и Details panel. Держатся здесь, в FHudStats, а не в
	// отдельной структуре, ровно потому, что виджету так нужен один Break-нод
	// вместо двух, и потому же, что сюда уже переехали заданные Speed/
	// StepsPerRender - "настройка" и "измерение" в этой сводке живут рядом
	// осознанно (см. SimulationSpeed).

	/** Идёт непрерывный прогон (P) - см. bSimulationRunning. Взаимоисключающ с
	 *  bFastStepActive ниже. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bSimulationRunning = false;

	/** Держат Shift+F - автошаг, см. bFastStepActive/StartFastStep(). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bFastStepActive = false;

	/** Режим взаимодействия мышью (Tab): камера стоит, курсор виден, работают
	 *  рамка выделения и клики по HUD. Читается с контроллера
	 *  (AGamePlayerController::IsSelectionModeActive()), а не хранится тут -
	 *  false, если контроллер ещё не готов. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bSelectionModeActive = false;

	/** CPU или GPU - см. ComputeMethod. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	EComputeMethod ComputeMethod = EComputeMethod::Cpu;

	/** Разлитый по кадрам рендер (Z) - см. bEnableChunkedRender. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bChunkedRenderEnabled = false;

	/** Порядок появления клеток при разливе (X) - см. EChunkedRenderOrder. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	EChunkedRenderOrder ChunkedRenderOrder = EChunkedRenderOrder::Sequential;

	/** Ждать дорисовки разлива перед следующим шагом (V) - см.
	 *  bWaitForChunkedRenderToFinish. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bWaitForChunkedRenderToFinish = false;

	/** Отсечение клеток по расстоянию (B) - см. bEnableCellCulling. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bCellCullingEnabled = false;

	/** Отсечение кубом включено (C) - см. bEnableRenderCullVolume. Само по себе
	 *  НЕ означает, что клетки сейчас режутся: см. bCullVolumeActive ниже. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bRenderCullVolumeEnabled = false;

	/** Куб виден (Ctrl+C) - ARenderCullVolume::IsVolumeVisible(). false и в
	 *  случае, когда ARenderCullVolume на уровне просто нет. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bRenderCullVolumeVisible = false;

	/** Итог: клетки СЕЙЧАС реально режутся кубом - конъюнкция "актёр есть на
	 *  уровне И включено (C) И видно (Ctrl+C)", т.е. ровно то, что отвечает
	 *  GetActiveCullVolume(). Отдельное поле, а не "И" двух предыдущих в
	 *  виджете: наличие актёра на уровне из них не выводится, а именно оно и
	 *  делает разницу между "включено" и "работает". */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bCullVolumeActive = false;

	/** Chunk-силуэт (H) - см. bEnableGhostShape. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bGhostShapeEnabled = false;

	/** Силуэт сейчас ЗАМЕНЯЕТ поклеточный рендер, а не дополняет его - см.
	 *  ShouldGhostShapeReplaceDetailedRender(). Именно этот режим и даёт
	 *  выигрыш в скорости, поэтому его видно отдельно от bGhostShapeEnabled. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bGhostShapeReplacesDetailedRender = false;
};

UCLASS()
class CELLULARAUTOMATA_API AAutomataOrchestrator : public AActor
{
	GENERATED_BODY()

public:
	AAutomataOrchestrator();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	/** Правки CellMeshComponentType должны сразу перепривязать CellsRenderer, а
	 *  правки цветовой рампы (CellMaterial/AgeColors/AgeColorMaxAge/
	 *  DecayColors) - сразу перерисовать текущее поколение: цвет это чистая
	 *  функция уже посчитанного состояния, ждать следующего шага симуляции
	 *  (а на паузе - вообще неизвестно чего) незачем. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

public:
	virtual void Tick(float DeltaTime) override;

	/** Запустить непрерывную симуляцию */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata")
	void Start();

	/** Поставить симуляцию на паузу */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata")
	void Pause();
	
	/** Возобновить */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata")
	void Resume();

	/** Остановить и сбросить симуляцию */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata")
	void Stop();

	/** Идёт ли сейчас непрерывная симуляция (между Start() и Stop()) - нужно
	 *  внешнему коду (например, хоткею в AGamePlayerController), чтобы
	 *  решить, звать Start() или Stop(), не трогая bSimulationRunning напрямую. */
	UFUNCTION(BlueprintPure, Category = "Automata")
	bool IsSimulationRunning() const { return bSimulationRunning; }

	/** Включает автошаг "как Play" (Shift+F, пока зажато - см.
	 *  AGamePlayerController::OnFastStepPressed()) - темп по Speed, та же
	 *  ветка Tick(), что и bSimulationRunning, но останавливается по
	 *  отпусканию клавиши (OnFastStepReleased()), а не повторным нажатием.
	 *  Отказывает (с warning в лог), если уже идёт Play (bSimulationRunning) -
	 *  F и P не работают одновременно, симметрично тому, как Start() ниже
	 *  отказывает, если активен автошаг. Обычное нажатие F без Shift сюда не
	 *  попадает вовсе - оно просто вызывает Next() один раз. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void StartFastStep();

	/** Останавливает автошаг, запущенный StartFastStep(). */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void StopFastStep();

	/** Активен ли сейчас автошаг Shift+F - нужно AGamePlayerController::
	 *  OnFastStepReleased(), чтобы понять, есть ли что останавливать. */
	UFUNCTION(BlueprintPure, Category = "Automata")
	bool IsFastStepActive() const { return bFastStepActive; }

	/** Вычисляет мировой центр и радиус описанной сферы вокруг всех живых
	 *  клеток - нужно AGamePlayerController::OnFrameAllCells(), чтобы
	 *  поставить камеру на расстояние, при котором вся сетка помещается в
	 *  кадр. Возвращает false (Out-параметры не трогает), если сетка не
	 *  инициализирована или пуста. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	bool ComputeAliveCellsBounds(FVector& OutCenter, float& OutRadius) const;

	/** Выполнить ручной шаг симуляции (хоткей F): считает StepsPerRender
	 *  поколений подряд (то же значение, что крутится хоткеями T/G) и
	 *  рендерит только итоговое, одним снимком - при StepsPerRender == 1
	 *  это прежний одиночный шаг. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata")
	void Next();
	
	/** Сгенерировать новое случайное состояние */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata")
	void Clear();
	
	/** Сгенерировать новое случайное состояние */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata")
	void GenerateRandom();

	/** Сгенерировать новое случайное состояние с новым сидом */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata")
	void NewSeed();

	/** Отладочная проверка корректности правила - сажает три классических
	 *  плоских 2D-паттерна (блок-неподвижку, мигалку-осциллятор, планер) в
	 *  одном Z-слое, на достаточном расстоянии друг от друга, чтобы не
	 *  взаимодействовали (радиус влияния Moore - 1 клетка, между паттернами
	 *  оставлено по 6+ клеток пустоты). Смысл теста: пока соседние Z-слои
	 *  пусты, подсчёт соседей по Moore-26 в 3D математически совпадает с
	 *  обычным 2D Moore-8 (эти 18 "лишних" соседей выше/ниже всегда дают 0),
	 *  так что при BirthCounts={3}/SurvivalCounts={2,3}/Neighborhood=Moore
	 *  (классическое правило Конвея) все три паттерна обязаны вести себя
	 *  ТОЧНО как в оригинальной 2D игре "Жизнь": блок не меняется вообще,
	 *  мигалка каждый шаг переключается между горизонтальной и вертикальной
	 *  тройкой, планер идентичной формой сдвигается по диагонали на (1,1)
	 *  каждые 4 поколения. Расхождение с любым из трёх - прямой сигнал бага
	 *  в подсчёте соседей/применении правила, а не просто "чужое 3D-правило
	 *  не совпало" (см. обсуждение проверки корректности автомата). Сама
	 *  функция НЕ трогает BirthCounts/SurvivalCounts/Neighborhood - их нужно
	 *  выставить в Details panel перед запуском (та же логика, что и у
	 *  остальных методов - Details panel остаётся единственным источником
	 *  правды для правила). Как и GenerateRandom(), пересоздаёт Grid с нуля,
	 *  сбрасывает выделение/счётчик поколений/запечённые меши и записывает
	 *  паттерн в InitialStateCells, так что R воспроизводит его заново. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|Debug")
	void SpawnRuleVerificationPattern();

	/** Выбирает живые клетки, чья экранная проекция попадает в прямоугольник
	 *  [RectMin, RectMax] (без ограничения по глубине - см. CellSelection::
	 *  SelectCellsInScreenRect()), комбинирует результат с текущим SelectedCells
	 *  по CombineMode (заменить/добавить/убрать - Shift/Ctrl-модификаторы
	 *  драга, см. ESelectionCombineMode) и сразу перерисовывает подсветку
	 *  (RenderSelectionOverlay()), не дожидаясь следующего шага симуляции.
	 *  Матрицу вида-проекции строит вызывающий код
	 *  (AGamePlayerController::OnSelectDragFinished()) один раз на всю
	 *  операцию, не на клетку. Если активен куб отсечения (bEnableRenderCullVolume
	 *  и есть ARenderCullVolume на уровне) - кандидатов на выделение сначала
	 *  сужаем до клеток ВНУТРИ его границ (Grid->GetAliveCellsInBounds() +
	 *  FFilteredCellGridView, тот же приём отсечения, что BuildCellRenderData()
	 *  использует для рендера), иначе марки могли выделить клетки, которые физически не
	 *  видны на экране (спрятаны кубом) - выделение обязано ловить ровно то
	 *  подмножество, которое реально нарисовано, не всю сетку целиком. */
	UFUNCTION(BlueprintCallable, Category = "Automata|Selection")
	void SelectCellsInScreenRect(const FMatrix& ViewProjectionMatrix, const FVector2D& ViewportSize, const FVector2D& RectMin, const FVector2D& RectMax, ESelectionCombineMode CombineMode = ESelectionCombineMode::Replace);

	/** Выбор одиночной клетки кликом (не драгом): луч - депроецированный
	 *  курсор мыши (AGamePlayerController::OnSelectDragFinished() различает
	 *  клик и драг по сдвигу мыши, см. ClickDragThresholdPixels) - идёт через
	 *  решётку voxel-DDA (CellSelection::PickCellAlongRay()), первая живая
	 *  клетка на пути и есть "клетка под курсором". Результат (одна клетка
	 *  или ни одной, если кликнули в пустоту) комбинируется с текущим
	 *  SelectedCells по тому же CombineMode, что и прямоугольник: клик -
	 *  заменить (пустой клик очищает выделение), Shift+клик - добавить,
	 *  Ctrl+клик - убрать. Тот же принцип ограничения кубом отсечения, что и
	 *  у SelectCellsInScreenRect() (см. её doc-comment) - если куб активен,
	 *  DDA-обход луча идёт по FFilteredCellGridView, ограниченному границами
	 *  куба (её переопределённый IsAlive() - см. doc-comment
	 *  FFilteredCellGridView), так что клик "видит" ровно то, что реально
	 *  нарисовано, а не всю сетку насквозь. */
	UFUNCTION(BlueprintCallable, Category = "Automata|Selection")
	void SelectCellUnderCursor(const FVector& RayOrigin, const FVector& RayDirection, ESelectionCombineMode CombineMode = ESelectionCombineMode::Replace);

	/** Выделяет все живые клетки внутри текущих границ ARenderCullVolume
	 *  целиком (Grid->GetAliveCellsInBounds(CullVolume->GetWorldBounds())),
	 *  независимо от экранной проекции - в отличие от SelectCellsInScreenRect(),
	 *  это не марки по экрану, а прямой запрос "всё, что попадает в объём".
	 *  Работает по фактическим границам куба, даже если сам
	 *  bEnableRenderCullVolume сейчас выключен (куб - это в первую очередь
	 *  пространственная область на уровне, отсечение рендера - лишь одно из
	 *  её применений). Комбинируется с текущим SelectedCells по тому же
	 *  CombineMode, что и остальные методы выделения (хоткей K соседствует с
	 *  MoveCullVolumeToSelection() - тот же дух "куб <-> выделение" в обе
	 *  стороны). Отказывает (с warning в лог), если нет сетки или на уровне
	 *  нет ARenderCullVolume. */
	UFUNCTION(BlueprintCallable, Category = "Automata|Selection")
	void SelectCellsInCullVolume(ESelectionCombineMode CombineMode = ESelectionCombineMode::Replace);

	/** Делает клетки из SelectedCells единственным содержимым новой сетки
	 *  (возраст сброшен - как только что родившиеся) и выходит из режима
	 *  выделения. Мировые координаты НЕ переносятся к началу координат -
	 *  клетки остаются там же, где их выделили (правила автомата
	 *  трансляционно инвариантны, а камера и так уже смотрит именно туда).
	 *  Отказывает (с warning в лог), если ничего не выделено - тот же паттерн
	 *  guard'ов, что и у остальных методов этого класса. Дополнительно
	 *  запоминает извлечённые клетки в InitialStateCells - именно к этому
	 *  состоянию (а не к новому случайному) вернёт последующий вызов
	 *  ResetToInitialState() (хоткей R). */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|Selection")
	void StartFromSelection();

	/** Запекает текущее состояние в один цельный меш (хоткей M): только
	 *  наружные грани (face culling, см. CellMeshBuilder::BuildFromCells()),
	 *  внутренность полая. Если есть активное выделение (SelectedCells) -
	 *  запекается только оно, иначе все живые клетки. После запекания
	 *  инстансы-кубики И САМА СЕТКА выгружаются из памяти (Grid.Reset()) -
	 *  это снимок-"скульптура" для осмотра больших областей, продолжить
	 *  симуляцию с него нельзя; R (ResetToInitialState()) убирает меш и
	 *  начинает новый прогон. Play/автошаг останавливаются принудительно.
	 *  Материал - BakedMeshMaterial. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|Baking")
	void BakeCellsToMesh();

	/** Материал запечённого меша (см. BakeCellsToMesh()). Не обязателен, но
	 *  фолбэка больше нет: при неназначенном будет warning в лог и дефолтный
	 *  материал движка. Раньше подставлялся AgeMaterials[0], но подставить
	 *  сюда CellMaterial нельзя - он красит клетки через per-instance custom
	 *  data, которых у UProceduralMeshComponent нет, и меш вышел бы ЧЁРНЫМ,
	 *  молча. Серый дефолт движка плюс строчка в логе честнее. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Baking")
	UMaterialInterface* BakedMeshMaterial = nullptr;

	/** Эксперимент (как и Baking выше): грубый "призрачный" силуэт живых
	 *  клеток, построенный по занятым ЧАНКАМ (не клеткам) - один кубик
	 *  геометрии на чанк вместо одного на клетку, на порядки дешевле
	 *  точного BakeCellsToMesh(). В отличие от него - НЕ одноразовый снимок:
	 *  сосуществует с живой симуляцией, Grid не освобождается, пересчитывается
	 *  на лету раз в GhostShapeRefreshInterval поколений (см. RefreshGhostShape()).
	 *
	 *  Два режима, в зависимости от bEnableRenderCullVolume и наличия
	 *  ARenderCullVolume на уровне: (1) куб активен - силуэт строится ТОЛЬКО
	 *  по чанкам СНАРУЖИ куба, детальный поклеточный рендер
	 *  (BuildCellRenderData()) по-прежнему рисует всё, что внутри - силуэт
	 *  здесь чистое дополнение;
	 *  (2) куба нет или он выключен - "снаружи" значит "везде": силуэт
	 *  покрывает всю сетку целиком и в этом случае ПОЛНОСТЬЮ ЗАМЕНЯЕТ
	 *  детальный рендер (см. ShouldGhostShapeReplaceDetailedRender()) -
	 *  именно так эта фича экономит время при большом числе живых клеток,
	 *  когда сам детальный путь (сбор + AddInstances по каждой клетке)
	 *  становится дорогим, а нужен лишь общий силуэт того, как развивается
	 *  структура. Выключено по умолчанию. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|GhostShape")
	bool bEnableGhostShape = false;

	/** Раз в сколько реально посчитанных поколений пересчитывать
	 *  ghost-форму - CellMeshBuilder::BuildFromCells() синхронный и
	 *  однопоточный (как и в точном bake), пересчёт на КАЖДОЕ поколение
	 *  при большом числе занятых чанков может быть заметен на game thread,
	 *  поэтому не каждый раз, как и StepsPerRender для основного рендера,
	 *  но отдельным счётчиком (см. GhostShapeGenerationsSinceRefresh). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|GhostShape",
			  meta = (ClampMin = "1", EditCondition = "bEnableGhostShape"))
	int32 GhostShapeRefreshInterval = 10;

	/** Материал ghost-меша (полупрозрачный "призрак" - см. bEnableGhostShape).
	 *  Не обязателен, но фолбэка больше нет - warning в лог и дефолтный
	 *  материал движка, ровно как у BakedMeshMaterial (там же причина). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|GhostShape",
			  meta = (EditCondition = "bEnableGhostShape"))
	UMaterialInterface* GhostShapeMaterial = nullptr;

	/** "Сохранить" (хоткей Ctrl+S, стандартная комбинация): если в этой
	 *  сессии уже есть путь от предыдущего SaveStateAs()/LoadStateFromFile()
	 *  (LastSaveFilePath) - перезаписывает его БЕЗ диалога; иначе не знает,
	 *  куда писать, и делегирует в SaveStateAs() (т.е. первый Ctrl+S всё
	 *  равно спросит путь один раз - дальше тихо перезаписывает). В файл
	 *  идёт ИЗНАЧАЛЬНЫЙ паттерн (InitialStateCells - см. подробности в
	 *  doc-comment WriteStateToFile()), а миниатюра - скриншот ТЕКУЩЕГО вида
	 *  (какая сейчас камера и какая сейчас живая симуляция на экране); сама
	 *  сетка при этом НИКАК не трогается - Guard'а bStepInProgress здесь нет
	 *  сознательно: сохранение только читает InitialStateCells (плюс
	 *  снимает уже отрисованный кадр), не мутирует Grid, а InitialStateCells
	 *  пишут только StartFromSelection()/LoadStateFromFile()/GenerateRandom()
	 *  (см. её doc-comment) - все строго на game thread, том же потоке, что
	 *  и эта функция. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|SaveLoad")
	void SaveState();

	/** "Сохранить как" (хоткей Ctrl+Shift+S): ВСЕГДА открывает системный
	 *  диалог "Сохранить как" (по умолчанию Saved/AutomataSaves/), даже если
	 *  LastSaveFilePath уже известен - в отличие от SaveState(). В файл идёт
	 *  ИЗНАЧАЛЬНЫЙ паттерн, миниатюра - скриншот текущего вида - см.
	 *  WriteStateToFile(). Формат - JSON-шапка (FAutomatonSaveHeader) +
	 *  бинарная полезная нагрузка клеток + PNG-миниатюра (см.
	 *  AutomatonStateSerializer). Успешная запись обновляет LastSaveFilePath,
	 *  так что последующий Ctrl+S будет тихо перезаписывать этот файл. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|SaveLoad")
	void SaveStateAs();

	/** "Открыть" (хоткей Ctrl+O): системный диалог "Открыть", затем
	 *  применяет параметры из JSON-шапки к UPROPERTY (правила,
	 *  CellSize/ChunkSize/GridSize, параметры генерации - СТРОГО до
	 *  CreateGrid(), который их читает), пересоздаёт сетку и заливает клетки
	 *  с их сохранёнными возрастами. Play/автошаг принудительно
	 *  останавливаются (как в BakeCellsToMesh()); guard bStepInProgress здесь
	 *  ОБЯЗАТЕЛЕН (в отличие от SaveState()/SaveStateAs() - те не трогают
	 *  Grid вовсе) - загрузка СВАПАЕТ Grid, который фоновый шаг может
	 *  читать в этот момент. До успешного разбора файла никакое состояние не
	 *  трогается - любой отказ (не тот файл, версия новее, порча) безопасен.
	 *  InitialStateCells (точка возврата R, и одновременно то, что уйдёт в
	 *  файл при следующем Save - см. WriteStateToFile()) восстанавливается
	 *  ИЗ ФАЙЛА как отдельный раздел (см. AutomatonStateSerializer.h) - не
	 *  выводится заново из загруженного снимка, иначе R после загрузки
	 *  возвращал бы не к изначальному паттерну, а к уже
	 *  проэволюционировавшему состоянию на момент сохранения. R реиграет с
	 *  возрастами 0, как обычно; точные возрасты снимка - повторный Ctrl+O.
	 *  Успешная загрузка тоже обновляет LastSaveFilePath - последующий
	 *  Ctrl+S перезапишет именно загруженный файл. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|SaveLoad")
	void LoadStateFromFile();

	/** Сторона (ширина = высота) PNG-миниатюры сохранения (см.
	 *  CaptureThumbnailPng()) - снятый с вьюпорта скриншот сперва обрезается
	 *  до квадрата по центру (короткая сторона вьюпорта целиком, длинная -
	 *  симметрично по краям), затем БЕЗУСЛОВНО масштабируется до РОВНО этого
	 *  значения с обеих сторон - единый стандартный квадратный размер для
	 *  всех сохранений независимо от текущего разрешения/соотношения сторон
	 *  окна, а не просто "не больше чем". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|SaveLoad", meta = (ClampMin = "32", UIMin = "128", UIMax = "2048"))
	int32 ThumbnailSizePixels = 768;

	/** Убивает выделенные клетки (хоткей Delete): каждая клетка из
	 *  SelectedCells становится мёртвой прямо в текущей сетке, выделение
	 *  сбрасывается, сетка перерисовывается немедленно. Симуляция при этом
	 *  НЕ трогается - это ручная правка состояния (вырезать кусок паттерна и
	 *  посмотреть, как он поведёт себя дальше), а не сброс. Отказывает (с
	 *  warning), пока фоновый шаг читает сетку - тот же guard, что у всех
	 *  мутаций Grid. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|Selection")
	void DeleteSelectedCells();

	/** Инвертирует текущее выделение относительно живых клеток (хоткей I):
	 *  выделенные становятся невыделенными, все остальные живые - выделенными.
	 *  Пустое выделение после инверсии = "выделить всё" - это осознанно
	 *  (стандартная семантика инверсии), а не ошибка. Сразу перерисовывает
	 *  подсветку, как и SelectCellsInScreenRect(). */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|Selection")
	void InvertSelection();

	/** Хоткей R: пересоздаёт сетку заново из InitialStateCells (возраст снова
	 *  сброшен в 0) - "точки возврата", заполненной последним
	 *  StartFromSelection() (Enter), LoadStateFromFile() (Ctrl+O) или
	 *  GenerateRandom() (в т.ч. автоматическим вызовом из BeginPlay - см.
	 *  doc-comment InitialStateCells) - то есть R "сброс к тому состоянию,
	 *  которое сейчас числится изначальным", будь то выбранный паттерн,
	 *  загруженный файл или просто последняя случайная генерация. Если
	 *  InitialStateCells пуст (на практике недостижимо в обычном потоке -
	 *  BeginPlay сам генерирует) - делегирует в GenerateRandom(). Камера НЕ
	 *  кадрируется автоматически (раньше кадрировалась, как Home, но R -
	 *  частое повторяющееся действие в процессе поиска паттернов, а
	 *  навязчивое авто-кадрирование при каждом нажатии только мешало -
	 *  убрано по явной просьбе; кадрирование по-прежнему доступно отдельным
	 *  нажатием Home). */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|Selection")
	void ResetToInitialState();

	/** Цвет подсветки выделенных клеток - отдельный рендер-проход поверх
	 *  обычного рендера (см. SelectionMeshComponent/SelectionRenderer),
	 *  рисуется тем же CellMesh и тем же CellMaterial, что и обычные клетки,
	 *  отличаясь только цветом в per-instance custom data (раньше это был
	 *  отдельный SelectionMaterial - убран вместе с AgeMaterials, чтобы во
	 *  всём проекте остался ровно один материал клеток). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Selection")
	FLinearColor SelectionColor = FLinearColor(0.0f, 1.0f, 0.2f, 1.0f);

	/** Во сколько раз кубик подсветки выделения крупнее обычного кубика
	 *  клетки. Ровно 1.0 нельзя: кубик подсветки тогда совпадает с обычным
	 *  поверхность-в-поверхность и мерцает (z-fighting) - поэтому по
	 *  умолчанию 1.1: подсветка обволакивает клетку и видна с любого угла.
	 *  Читается заново на каждый RenderSelectionOverlay(), без кэширования -
	 *  как и остальные параметры рендера. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Selection",
			  meta = (ClampMin = "1.0", UIMax = "1.5"))
	float SelectionScaleMultiplier = 1.1f;

	/** Скорость симуляции (шагов в секунду) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata",
			  meta = (ClampMin = "0.1", UIMin = "0.1", UIMax = "10.0"))
	float Speed = 5.0f;

	/** Меняет Speed на Delta (например, из хоткеев +/- в
	 *  AGamePlayerController), клампится к [0.1, 100.0] - шире, чем UIMax
	 *  в UPROPERTY-метаданных Speed выше (тот ограничивает только слайдер
	 *  в Details panel, не сам ClampMax). */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void AdjustSpeed(float Delta);

	/** Абсолютная установка Speed с тем же клампом [0.1, 100.0], что и
	 *  AdjustSpeed() - для слайдера в HUD, который задаёт значение целиком, а
	 *  не дельтой. Через сеттер, а не прямой записью в BlueprintReadWrite
	 *  UPROPERTY, именно ради кламп[а]: слайдер с чуть более широким
	 *  диапазоном иначе смог бы выставить 0 и поделить на ноль в Tick()
	 *  (там стоит защита через KINDA_SMALL_NUMBER, но полагаться на неё как
	 *  на штатный путь не стоит). */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void SetSpeed(float NewSpeed);

	/** Множитель скорости полёта камеры при удержании Shift (см.
	 *  AGamePlayerController::OnSpeedBoostStarted() - камера летает через
	 *  ADefaultPawn/UFloatingPawnMovement, эта настройка живёт здесь, а не
	 *  на самом контроллере, чтобы дизайнер тюнил её из того же Details
	 *  panel, что и остальную симуляцию). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Camera",
			  meta = (ClampMin = "1.0", UIMin = "1.0", UIMax = "10.0"))
	float CameraSpeedMultiplier = 6.0f;

	/** Размер сетки в клетках по осям X, Y, Z */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Grid",
			  meta = (ClampMin = "1", UIMin = "10", UIMax = "500"))
	FIntVector GridSize = FIntVector(100, 100, 100);

	/** Размер стороны чанка (в клетках) для сетки: чанк хранит ChunkSize^3
	 *  клеток как плотный битовый массив. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Grid",
			  meta = (ClampMin = "1", UIMin = "4", UIMax = "64"))
	int32 ChunkSize = 16;

	/** Меш, используемый для отрисовки одной клетки автомата (инстансированный) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	UStaticMesh* CellMesh = nullptr;

	/** Базовый материал клеток - ЕДИНСТВЕННЫЙ, один на все клетки любого
	 *  возраста. Цвет задаётся НЕ материалом, а per-instance custom data
	 *  (см. AgeColors/DecayColors ниже): рендерер пишет RGB в 3 float'а на
	 *  инстанс, а материал ОБЯЗАН читать их узлом PerInstanceCustomData3Vector
	 *  (DataIndex=0) и вести в Base Color / Emissive.
	 *  ВНИМАНИЕ: без такого узла движок вообще не создаёт буфер custom data
	 *  (флаг выводится из самого материала) - все клетки выйдут одного цвета,
	 *  БЕЗ единой ошибки в логе. Полезно задать узлу заметный ConstDefaultValue
	 *  (например пурпурный), чтобы "custom data не доехали" отличалось от
	 *  "цвет действительно чёрный".
	 *  ОБЯЗАТЕЛЕН - без него GenerateRandom()/Next()/StepAsync() откажутся
	 *  выполняться с warning в лог, ровно как и без CellMesh.
	 *  Пришёл на смену массиву AgeMaterials: материал в
	 *  UInstancedStaticMeshComponent/HISM задаётся на компонент целиком, а не
	 *  на инстанс, поэтому N материалов требовали N отдельных компонентов с
	 *  бакетизацией клеток по возрасту; per-instance цвет схлопывает всё это в
	 *  один компонент и один AddInstances() на поколение. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	UMaterialInterface* CellMaterial = nullptr;

	/** Ключевые кадры цвета по возрасту клетки, с ПЛАВНОЙ интерполяцией между
	 *  соседними ключами (а не ступенями, как было с AgeMaterials).
	 *  Порядок: [0] - только что родившаяся клетка (возраст 0), последний -
	 *  самая старая (возраст >= AgeColorMaxAge).
	 *  ВНИМАНИЕ: это ОБРАТНЫЙ порядок относительно прежнего AgeMaterials, где
	 *  первым шёл самый СТАРЫЙ материал. Пустой массив - все клетки белые (т.е.
	 *  "как выглядит сам материал"), с предупреждением в лог; один элемент -
	 *  плоский цвет независимо от возраста.
	 *  Пустота намеренно НЕ считается ошибкой, в отличие от отсутствующего
	 *  CellMaterial: это нормальное промежуточное состояние настройки (материал
	 *  уже назначен, рампа ещё не нарисована), и отказ рисовать в этот момент
	 *  выглядел бы как "ничего не работает". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	TArray<FLinearColor> AgeColors;

	/** Возраст, на котором достигается ПОСЛЕДНИЙ ключевой кадр AgeColors, то
	 *  есть сама шкала интерполяции: t = clamp(Age / AgeColorMaxAge, 0, 1).
	 *  Клетки старше красятся последним цветом.
	 *  Отвязано от AgeColors.Num() специально: число ключей - это разрешение
	 *  градиента, а этот параметр - его длина по времени. У AgeMaterials это
	 *  было одним и тем же числом (N материалов = N возрастов), и настроить
	 *  "мягкий градиент, но за 4 поколения" было нельзя.
	 *  Возраст в FCellGrid насыщающийся uint8, так что 255 - потолок при любом
	 *  значении здесь. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "255"))
	int32 AgeColorMaxAge = 32;

	/** Отдельная шкала цвета для УГАСАЮЩИХ клеток Generations (States > 2,
	 *  см. FCellGrid::IsDecaying()): [0] - только начала угасать (DecayState 2),
	 *  последний - последняя стадия перед смертью (DecayState States-1).
	 *  ПУСТОЙ (по умолчанию) - угасающие красятся по AgeColors. Раньше выбора
	 *  не было вообще: угасающие клетки шли в те же возрастные бакеты, что и
	 *  живые, и были от них визуально неотличимы - хотя именно угасающая
	 *  оболочка и есть самое интересное в правилах Generations. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	TArray<FLinearColor> DecayColors;

	/** Размер одной клетки в мировых единицах */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "1.0", UIMin = "1.0", UIMax = "1000.0"))
	float CellSize = 100.0f;

	/** Какая реализация инстансированного компонента используется для
	 *  отрисовки клеток - выбирает, какой из двух постоянно существующих
	 *  компонентов (CellsMeshFlat/CellsMeshHierarchical) реально получает
	 *  инстансы, см. GetActiveCellsMeshComponent(). Класс компонента при этом
	 *  не подменяется - оба созданы через CreateDefaultSubobject и живут всю
	 *  жизнь актора (менять класс default subobject'а в рантайме Live Coding
	 *  безопасно не умеет, проект на этом уже обжигался - см. CLAUDE.md).
	 *  Правка перепривязывает CellsRenderer на другой компонент и чистит
	 *  инстансы у прежнего (см. EnsureCellsRenderer()).
	 *
	 *  По умолчанию ПЛОСКИЙ Instanced, а не HierarchicalInstanced - это
	 *  замер, а не вкус. Оба прогнаны на одной и той же сетке (одинаковый
	 *  Seed, значит структура побитово та же), правило 0-6/1,3/2/VN, куб
	 *  отсечения и отсечение по расстоянию выключены:
	 *
	 *      клеток     FPS: ISM -> HISM     AddInstances: ISM -> HISM
	 *      100 881       76.0 -> 84.0            4.6 -> 10.2 мс
	 *      508 281       10.1 ->  8.9           25.3 -> 37.9 мс
	 *      990 055        4.8 ->  4.5           49.5 -> 76.0 мс
	 *
	 *  Вставка у HISM дороже в 1.5-2.3 раза на всех размерах, а выигрыша по
	 *  FPS нет (разброс в 10-25% сопоставим с влиянием положения камеры, так
	 *  что утверждать вред нельзя - но и пользы не видно). Причина в том, что
	 *  дерево кластеров LOD не успевает пригодиться: рендер делает
	 *  ClearInstances()+AddInstances() КАЖДОЕ поколение, дерево строится
	 *  заново и асинхронно, и к следующему поколению чаще всего ещё не
	 *  готово - отсечение просто не включается. Плюс типичная структура здесь
	 *  плотная и выпуклая, окклюзии между кластерами почти нет.
	 *
	 *  HISM имеет смысл вернуть, если появится режим, где сетка ДОЛГО не
	 *  меняется (пауза, осмотр запечённого состояния) - тогда дерево успеет
	 *  построиться и начнёт окупаться. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	ECellMeshComponentType CellMeshComponentType = ECellMeshComponentType::Instanced;

	/** Расстояние от камеры, на котором инстансы клеток начинают исчезать -
	 *  см. CellCullEndDistance ниже (UInstancedStaticMeshComponent::
	 *  InstanceStartCullDistance/SetCullDistances()). 0 - отсечение по
	 *  расстоянию выключено целиком (стандартное поведение движка, было до
	 *  добавления этих двух параметров). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "0.0"))
	float CellCullStartDistance = 0.0f;

	/** Расстояние от камеры, дальше которого инстансы клеток вообще не
	 *  рисуются - честное runtime-отсечение по расстоянию
	 *  (UInstancedStaticMeshComponent::InstanceEndCullDistance/
	 *  SetCullDistances()), а не HLOD: HLOD рассчитан на запечённые
	 *  проксирующие меши статичных акторов (нужен отдельный шаг "Build
	 *  HLODs" в редакторе) и не годится для сетки клеток, которая
	 *  перестраивается целиком каждое посчитанное поколение - у HLOD просто
	 *  нет статичного состояния, которое можно было бы запечь. SetCullDistances()
	 *  применяется к CellsMeshFlat/CellsMeshHierarchical и к
	 *  SelectionMeshComponent в ApplyCellCullDistances() на каждый рендер (сама
	 *  no-op, если значения не изменились - см. её реализацию в движке), так
	 *  что правки в Details panel подхватываются немедленно, как и остальные
	 *  параметры рендера. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "0.0"))
	float CellCullEndDistance = 0.0f;

	/** Мастер-переключатель отсечения по расстоянию - если false,
	 *  ApplyCellCullDistances() применяет ко всем компонентам (0, 0) вместо
	 *  CellCullStartDistance/CellCullEndDistance, т.е. полностью отключает
	 *  SetCullDistances(), НЕ трогая сами значения дистанций - удобно
	 *  быстро сравнить "с отсечением/без", не теряя подобранные числа.
	 *  Переключается на лету через хоткей B (см.
	 *  AGamePlayerController::OnToggleCellCulling()) или Details panel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	bool bEnableCellCulling = true;

	/** Включено ли сейчас отсечение по расстоянию (см. bEnableCellCulling) -
	 *  нужно внешнему коду (хоткею B), чтобы решить, на что переключать, не
	 *  трогая bEnableCellCulling напрямую. */
	UFUNCTION(BlueprintPure, Category = "Automata")
	bool IsCellCullingEnabled() const { return bEnableCellCulling; }

	/** Включает/выключает отсечение по расстоянию (см. bEnableCellCulling).
	 *  Без CallInEditor - параметр уже редактируется напрямую как чекбокс. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void SetCellCullingEnabled(bool bEnabled);

	/** Мастер-переключатель отсечения клеток по ARenderCullVolume - если
	 *  выключено или актёра нет в уровне, BuildCellRenderData() рендерит все
	 *  живые клетки как раньше (никогда не "рендерит тихо ничего"). В
	 *  отличие от CellCullStartDistance/CellCullEndDistance (пост-хок,
	 *  на уже построенных инстансах), этот фильтр применяется ДО
	 *  построения FTransform/AddInstances - см. FCellGrid::
	 *  GetAliveCellsInBounds() и BuildCellRenderData(). Переключается на лету
	 *  через хоткей C (см. AGamePlayerController::OnToggleRenderCullVolume())
	 *  или Details panel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Rendering")
	bool bEnableRenderCullVolume = true;

	/** Включено ли сейчас отсечение по ARenderCullVolume (см.
	 *  bEnableRenderCullVolume) - нужно внешнему коду (хоткею C), чтобы
	 *  решить, на что переключать, не трогая bEnableRenderCullVolume
	 *  напрямую. */
	UFUNCTION(BlueprintPure, Category = "Automata")
	bool IsRenderCullVolumeEnabled() const { return bEnableRenderCullVolume; }

	/** Включает/выключает отсечение по ARenderCullVolume (см.
	 *  bEnableRenderCullVolume). Без CallInEditor - параметр уже
	 *  редактируется напрямую как чекбокс. Не inline (в отличие от
	 *  IsRenderCullVolumeEnabled() выше) - сразу зовёт
	 *  RefreshRenderCullVolume(), а не ждёт следующего посчитанного
	 *  поколения, тем же способом, что SetCellCullingEnabled() применяет
	 *  ApplyCellCullDistances() немедленно. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void SetRenderCullVolumeEnabled(bool bEnabled);

	/** Немедленно перерисовывает ТЕКУЩЕЕ состояние сетки (RenderGridImmediate(),
	 *  без пересчёта нового поколения) - публичный метод специально для
	 *  ARenderCullVolume::PostEditMove()/PostEditChangeProperty(), которые
	 *  зовут его извне класса, когда пользователь закончил двигать/
	 *  масштабировать куб отсечения, чтобы новые границы сразу отразились
	 *  на экране. Тоже зовётся из SetRenderCullVolumeEnabled(). No-op, если
	 *  сетка ещё не создана (до первого GenerateRandom()). */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void RefreshRenderCullVolume();

	/** Срез вдоль взгляда - показывать только клетки, лежащие в слое,
	 *  перпендикулярном направлению камеры.
	 *
	 *  Появился как замена повороту ARenderCullVolume, а не как отдельная
	 *  возможность. Задача была - рассмотреть грань структуры, а у правил
	 *  вроде 0-6/1,3/2/VN грани лежат в плоскостях x+y+z = const, то есть
	 *  под 45 градусов ко всем осям: осевой куб с ними не совмещается никаким
	 *  размером, а отсечение по расстоянию (CellCullStartDistance/
	 *  CellCullEndDistance) не помогает по двум независимым причинам - оно
	 *  радиальное (сфера, а не плоскость) и работает на стороне рендера, уже
	 *  после построения инстансов. Срез же перпендикулярен взгляду, а значит
	 *  ВСЕГДА параллелен тому, на что смотришь: камера и играет роль поворота,
	 *  и никаких ручек для угла не нужно вовсе.
	 *
	 *  Режется в BuildCellRenderData(), до построения трансформов - как куб, и
	 *  вместе с кубом (условия складываются, а не заменяют друг друга). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	bool bEnableViewSlice = false;

	/** Расстояние от КАМЕРЫ до середины среза вдоль направления взгляда.
	 *  Именно от камеры, а не от центра структуры: тогда срез едет вместе с
	 *  полётом ("покажи то, что передо мной"), а не стоит на месте, вокруг
	 *  которого надо облетать. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "0.0", EditCondition = "bEnableViewSlice"))
	float ViewSliceDistance = 3000.0f;

	/** Толщина слоя. Клетка остаётся, если её глубина вдоль взгляда попала в
	 *  ViewSliceDistance +- половину этого значения. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "1.0", EditCondition = "bEnableViewSlice"))
	float ViewSliceThickness = 1000.0f;

	/** На сколько должна сдвинуться камера, чтобы срез пересчитался. Срез
	 *  привязан к камере, поэтому строго говоря его надо перестраивать на
	 *  каждый её сдвиг - а это полное построение инстансов заново. Порог
	 *  гасит дрожание: на сотне тысяч отрисованных клеток пере-рендер стоит
	 *  единицы миллисекунд, на миллионах уже заметен, и платить за него из-за
	 *  микроскопических смещений мыши незачем. Поворот учитывается отдельно (см.
	 *  ViewSliceRotationThreshold). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "0.0", EditCondition = "bEnableViewSlice"))
	float ViewSliceCameraMoveThreshold = 100.0f;

	/** На сколько градусов должна повернуться камера, чтобы срез
	 *  пересчитался - см. ViewSliceCameraMoveThreshold. Поворот важнее
	 *  смещения: он меняет саму ПЛОСКОСТЬ среза, а не только его положение. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "0.0", EditCondition = "bEnableViewSlice"))
	float ViewSliceRotationThreshold = 2.0f;

	UFUNCTION(BlueprintPure, Category = "Automata")
	bool IsViewSliceEnabled() const { return bEnableViewSlice; }

	/** Включает/выключает срез и сразу перерисовывает - как
	 *  SetRenderCullVolumeEnabled(), по той же причине: ждать следующего
	 *  поколения незачем, а на паузе его и не будет. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void SetViewSliceEnabled(bool bEnabled);

	/** Двигает середину среза (хоткеи [ и ]) / меняет толщину (Shift+[ и
	 *  Shift+]). Обе клампятся и сразу перерисовывают. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void AdjustViewSliceDistance(float Delta);

	UFUNCTION(BlueprintCallable, Category = "Automata")
	void AdjustViewSliceThickness(float Delta);

	/** Двигает ARenderCullVolume так, чтобы он оказался отцентрован на
	 *  ПЕРВОЙ выделенной клетке (SelectedCells[0]) - хоткей K. Куб довольно
	 *  неудобно таскать гизмо через весь уровень вручную, особенно на
	 *  большой сетке - выделил интересующую клетку (Tab + ЛКМ/клик), нажал
	 *  K, куб телепортировался прямо туда. Меняет только location актёра,
	 *  размер (BoxExtent) не трогает. No-op с warning-логом, если
	 *  SelectedCells пуст или на уровне нет ARenderCullVolume - тот же
	 *  принцип "никогда не делать тихо ничего/наполовину", что у остальных
	 *  guard'ов. В конце сама зовёт RefreshRenderCullVolume() - программный
	 *  SetActorLocation() не триггерит ARenderCullVolume::PostEditMove()
	 *  (тот колбэк WITH_EDITOR-only и реагирует только на ручное
	 *  перетаскивание/правку в Details panel, а не на код, к тому же в PIE
	 *  его вообще нет), так что без явного вызова куб визуально не подхватил
	 *  бы новые границы до следующего шага симуляции. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void MoveCullVolumeToSelection();

	/** Включён ли сейчас Ghost Shape (см. bEnableGhostShape) - нужно внешнему
	 *  коду (хоткею), чтобы решить, на что переключать, не трогая
	 *  bEnableGhostShape напрямую. */
	UFUNCTION(BlueprintPure, Category = "Automata|GhostShape")
	bool IsGhostShapeEnabled() const { return bEnableGhostShape; }

	/** Включает/выключает Ghost Shape вручную (см. bEnableGhostShape). Без
	 *  CallInEditor - параметр уже редактируется напрямую как чекбокс. Не
	 *  inline (в отличие от IsGhostShapeEnabled() выше) - сразу зовёт
	 *  RefreshGhostShape() (пересчитает, если включили) или ClearGhostShape()
	 *  (снимет силуэт, если выключили), не дожидаясь следующего планового
	 *  GhostShapeRefreshInterval - тем же способом, что SetRenderCullVolumeEnabled()
	 *  применяет RefreshRenderCullVolume() немедленно. */
	UFUNCTION(BlueprintCallable, Category = "Automata|GhostShape")
	void SetGhostShapeEnabled(bool bEnabled);

	/** Метрики последнего BuildCellRenderData() (клетки/МБ, "отрисовано/всего") -
	 *  см. doc-comment FCellRenderStats. Читает уже посчитанное, ничего не
	 *  пересчитывает - используется и UE_LOG внутри BuildCellRenderData(), и
	 *  HUD (UMainHudWidget) через этот геттер. */
	UFUNCTION(BlueprintPure, Category = "Automata|Rendering")
	const FCellRenderStats& GetLastRenderStats() const { return LastRenderStats; }

	/** Фоновый StepAsync()/Next() сейчас считает поколение - см. doc-comment
	 *  bStepInProgress. Раньше был чисто внутренним guard'ом, теперь нужен и
	 *  наружу - HUD показывает это как индикатор занятости "считаем". */
	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	bool IsStepInProgress() const { return bStepInProgress; }

	/** Чанковый рендер сейчас "разливается" по кадрам - см. doc-comment
	 *  bChunkedRenderInProgress. HUD показывает это как индикатор занятости
	 *  "рисуем" (отдельно от "считаем" выше - это разные фазы, могут идти
	 *  параллельно, см. StepsPerRender/bRenderHandoffPending). */
	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	bool IsChunkedRenderInProgress() const { return bChunkedRenderInProgress; }

	/** Сводка для HUD (см. doc-comment FHudStats). Пересобирает снимок ПЕРЕД
	 *  выдачей, а не только отдаёт посчитанное в Tick() - и поэтому НЕ const.
	 *
	 *  Иначе HUD врёт всякий раз, когда состояние меняется, а актор не тикает:
	 *  тик включается только Start()/StartFastStep() и выключается Stop()
	 *  (см. bStartWithTickEnabled в конструкторе), так что ручной шаг F при
	 *  остановленном Play исправно увеличивал GenerationCount, но снимок для
	 *  HUD не обновлялся, и счётчик эпох на экране стоял на месте. То же
	 *  касалось SelectedCellCount при выделении мышкой на паузе. Обновление
	 *  здесь закрывает все такие пути разом, вместо того чтобы дописывать
	 *  вызов в каждый из них и забыть в следующем.
	 *
	 *  Дёшево даже при нескольких биндингах виджета на кадр: все поля - это
	 *  либо чтение уже готовых значений, либо Grid->Num() (счётчик в чанках,
	 *  O(1)); единственный нетривиальный кусок, UpdateGenerationsPerSecond(),
	 *  сам себя ограничивает окном в секунду. */
	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	const FHudStats& GetHudStats() { UpdateHudStats(); return LastHudStats; }

	/** Включает/выключает разлитый по кадрам рендер целиком (см.
	 *  ChunkedRenderCellsPerFrame) - если false, StepAsync() всегда рендерит
	 *  новую сетку одним кадром, как до появления чанкинга. Никакого
	 *  автоматического порога по числу клеток нет - переключается только
	 *  вручную (Details panel или хоткей Z, см.
	 *  AGamePlayerController::OnToggleChunkedRender()). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	bool bEnableChunkedRender = true;

	/** Включено ли сейчас разлитое по кадрам рендер - нужно внешнему коду
	 *  (хоткею Z), чтобы решить, на что переключать, не трогая
	 *  bEnableChunkedRender напрямую. */
	UFUNCTION(BlueprintPure, Category = "Automata")
	bool IsChunkedRenderEnabled() const { return bEnableChunkedRender; }

	/** Включает/выключает разлитый по кадрам рендер (см. bEnableChunkedRender). */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata")
	void SetChunkedRenderEnabled(bool bEnabled);

	/** Сколько инстансов добавлять за один Tick, пока идёт "разлитый" по
	 *  кадрам рендер. Актуально только при bEnableChunkedRender == true.
	 *
	 *  ЧЕМ БОЛЬШЕ, ТЕМ ДЕШЕВЛЕ СУММАРНО - это не просто нарезка фиксированной
	 *  работы по кадрам. Каждый вызов AddInstances() делает работу,
	 *  пропорциональную уже накопленному числу инстансов, поэтому N вызовов по
	 *  B клеток стоят заметно дороже одного вызова по N*B. Замерено на 622k
	 *  клеток (см. RenderTimings в логе, плоский Instanced-компонент):
	 *  1 вызов - 35.97 мс, 7 вызовов (бюджет 100k) - 78.63 мс, 31 вызов
	 *  (бюджет 20k) - 179.23 мс. То есть чанкинг честно срезает пик кадра, но
	 *  платит за это кратным ростом ОБЩЕЙ работы, и на больших сетках сделка
	 *  невыгодна: один провал в 36 мс лучше сорока кадров по 5 мс.
	 *  Значение поднято с 20000 до 100000 не ради использования чанкинга, а
	 *  чтобы случайное нажатие Z не било в пять раз по производительности. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "1", EditCondition = "bEnableChunkedRender", EditConditionHides))
	int32 ChunkedRenderCellsPerFrame = 100000;

	/** Метод по умолчанию, определяющий, в каком порядке живые клетки
	 *  появляются по кадрам "разлитого" рендера (см. bEnableChunkedRender/
	 *  EChunkedRenderOrder) - не влияет на однократный Render() (Next()/
	 *  GenerateRandom()), только на визуальный порядок реавила при
	 *  непрерывной симуляции. Без EditCondition (в отличие от
	 *  ChunkedRenderCellsPerFrame выше) - специально всегда виден в Details
	 *  panel, а не только когда bEnableChunkedRender включён, чтобы дизайнер
	 *  мог подобрать метод заранее, до включения чанкинга. Переключается на
	 *  лету: RenderCurrentGrid() читает это значение заново на каждый вызов,
	 *  без кэширования - как и остальные параметры симуляции. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	EChunkedRenderOrder ChunkedRenderOrder = EChunkedRenderOrder::Sequential;

	/** Переключает ChunkedRenderOrder на следующее значение по кругу (хоткей
	 *  X, см. AGamePlayerController::OnCycleChunkedRenderOrder()) - чтобы
	 *  подобрать порядок реавила на лету, не открывая Details panel. Без
	 *  CallInEditor (как и SetWaitForChunkedRenderToFinish ниже) - ChunkedRenderOrder
	 *  и так уже редактируется напрямую как обычный dropdown, отдельная
	 *  кнопка-функция рядом с ним только дублировала бы её в Details panel. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void CycleChunkedRenderOrder();

	/** Что делать, если очередной шаг досчитался, пока предыдущий чанковый
	 *  "разлив" ещё не дорисован: false (по умолчанию, текущее поведение) -
	 *  немедленно прервать недорисованный разлив и тут же перерисовать новое
	 *  состояние (см. `ApplyStepResult()`); true - не прерывать, а сначала
	 *  дождаться, пока `AdvanceChunkedRender()` полностью дорисует текущий
	 *  разлив, и только после этого сбросить/нарисовать новое состояние и
	 *  запустить расчёт следующего шага. Реализовано без изменений в
	 *  `ApplyStepResult()` - гейтится на входе, в `Tick()`: пока true и
	 *  `bChunkedRenderInProgress`, следующий `StepAsync()` просто не
	 *  запускается (ни в обычном Play, ни в автошаге Shift+F), поэтому
	 *  `ApplyStepResult()` физически не может увидеть незавершённый разлив в
	 *  этом режиме. Переключается на лету через хоткей V (см.
	 *  `AGamePlayerController::OnToggleWaitForChunkedRenderToFinish()`) или
	 *  Details panel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	bool bWaitForChunkedRenderToFinish = false;

	/** Включён ли сейчас режим "ждать разлив" (см. bWaitForChunkedRenderToFinish) -
	 *  нужно внешнему коду (хоткею V), чтобы решить, на что переключать, не
	 *  трогая bWaitForChunkedRenderToFinish напрямую. */
	UFUNCTION(BlueprintPure, Category = "Automata")
	bool IsWaitingForChunkedRenderToFinish() const { return bWaitForChunkedRenderToFinish; }

	/** Включает/выключает режим "ждать разлив" (см. bWaitForChunkedRenderToFinish).
	 *  Без CallInEditor (в отличие от SetChunkedRenderEnabled) - параметр уже
	 *  редактируется напрямую как чекбокс, отдельная кнопка с полем bWait
	 *  только дублировала бы её в Details panel. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void SetWaitForChunkedRenderToFinish(bool bWait);

	/** Сколько посчитанных поколений пропускать между рендерами: 1 = рендерить
	 *  каждое (текущее поведение), N>1 - рендерить только каждое N-ое, пока
	 *  остальные N-1 продолжают считаться в фоне без отрисовки. Если очередное
	 *  N-ое поколение досчиталось раньше, чем закончился "разлив" предыдущего
	 *  показанного (bEnableChunkedRender) - предыдущий разлив немедленно
	 *  прерывается, и рендер сразу перезапускается на новом состоянии (см.
	 *  ApplyStepResult()/RenderCurrentGrid()) - недорисованное предыдущее
	 *  поколение просто никогда не попадает на экран. Next()/GenerateRandom()
	 *  этот порог игнорируют - рендерят немедленно всегда. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "1", ClampMax = "4096"))
	int32 StepsPerRender = 1;

	/** Верхняя граница StepsPerRender. Снизу 1 очевидна (рендерить реже раза
	 *  в поколение бессмысленно), сверху границы раньше не было вовсе - она
	 *  понадобилась, когда появилось удвоение с клавиши (Shift+T), которое
	 *  без потолка ушло бы в переполнение int32 за десяток нажатий. Значение
	 *  заведомо избыточное: батчинг в FGpuComputeStrategy обрезается гораздо
	 *  раньше (halo равен размеру батча, объём растёт кубически), так что это
	 *  предохранитель, а не рабочий диапазон. */
	static constexpr int32 MaxStepsPerRender = 4096;

	/** Меняет StepsPerRender на Delta (хоткеи T и G в AGamePlayerController),
	 *  клампится к [1, MaxStepsPerRender]. */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void AdjustStepsPerRender(int32 Delta);

	/** Абсолютная установка StepsPerRender с тем же клампом, что и
	 *  AdjustStepsPerRender() - для слайдера в HUD (см. SetSpeed() выше про
	 *  то, зачем сеттер при BlueprintReadWrite-свойстве). */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void SetStepsPerRender(int32 NewStepsPerRender);

	/** Переход к следующей (bDouble) или предыдущей степени двойки - хоткеи
	 *  Shift+T и Shift+G. Именно к степени, а не умножение на два: если
	 *  текущее значение неровное (254, набранное шагом в единицу), удвоение
	 *  оставило бы его неровным, а так одно нажатие всегда приводит на 2^k.
	 *  Степени двойки здесь и есть интересные значения - см. doc-comment
	 *  AGamePlayerController::OnDoubleStepsPerRender(). */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void ScaleStepsPerRender(bool bDouble);

	/** Количество живых клеток при генерации */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Random",
			  meta = (ClampMin = "1"))
	int32 Amount = 1000;

	/** Радиус (в клетках) вокруг центра, в котором генерируются случайные клетки */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Random",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "500"))
	int32 SpawnRadius = 10;

	/** Сид для генератора случайных чисел */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Random", 
			  meta = (DisplayName = "Random Seed"))
	int32 Seed = 0;

	/** Фактор кластеризации (0 - равномерно, 1 - максимальная кластеризация) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Random",
			  meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float ClusterFactor = 0.7f;

	/** Количества живых соседей, при которых мёртвая клетка рождается. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Rules")
	TArray<int32> BirthCounts = { 3 };

	/** Количества живых соседей, при которых живая клетка выживает. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Rules")
	TArray<int32> SurvivalCounts = { 2, 3 };

	/** Тип соседства для подсчёта живых соседей: Von Neumann (6, грани)
	 *  или Moore (26, полный куб 3x3x3). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Rules")
	ENeighborhood Neighborhood = ENeighborhood::Moore;

	/** Общее число состояний клетки - 2 (дефолт) значит классический
	 *  бинарный автомат (жива/мертва), поведение не отличается от того, что
	 *  было до появления этого свойства. States > 2 включает режим
	 *  "Generations": клетка, переставшая выживать, не умирает сразу, а
	 *  угасает через промежуточные состояния 2, 3, ..., (States-1) - всё
	 *  это время её нельзя ни оживить (birth-immune), ни убить, она просто
	 *  на фиксированной скорости идёт к состоянию 0 (окончательно мертва).
	 *  Только состояние 1 (полностью живая) считается "живым соседом" для
	 *  подсчёта по правилу. См. RuleStringParser.h для точной семантики
	 *  (то же, что у сайта williamyang98/3D-Cellular-Automata и семейства
	 *  Golly "Generations"). ClampMax=255 не декоративен - угасающее
	 *  состояние хранится как uint8 (см. FDenseCellGrid). Само по себе это
	 *  свойство ничего не делает без остальной части реализации Generations -
	 *  см. FCellularAutomatonRule::GetStates()/HasDecayStates(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Rules",
			  meta = (ClampMin = "2", ClampMax = "255", UIMin = "2", UIMax = "64"))
	int32 States = 2;

	/** Строковая нотация правила в формате "Survival/Birth/States/
	 *  Neighborhood" (например "0-6/1,3/2/VN" - см. RuleStringParser.h для
	 *  полного описания синтаксиса) - удобный способ ввести чужое правило
	 *  одной строкой вместо ручной правки массивов BirthCounts/
	 *  SurvivalCounts по элементу. Само по себе ничего не меняет - нужно
	 *  нажать ApplyRuleString() (или CallInEditor-кнопку), которая парсит
	 *  строку и перезаписывает BirthCounts/SurvivalCounts/States/
	 *  Neighborhood ниже. После применения RuleString не остаётся
	 *  синхронизированным источником истины - это одноразовый ввод, а не
	 *  сериализуемая форма правила (тот же дух, что Seed не хранит саму
	 *  сгенерированную сетку). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Rules")
	FString RuleString;

	/** Разбирает RuleString (см. её doc-comment) и, при успехе, перезаписывает
	 *  BirthCounts/SurvivalCounts/States/Neighborhood выше - присваивает по
	 *  имени поля, не позиционно (порядок полей в строке - Survival, затем
	 *  Birth - не совпадает с порядком объявления BirthCounts/SurvivalCounts
	 *  здесь, слепое присваивание по позиции тихо перепутало бы их). При
	 *  ошибке разбора ничего не меняет и пишет warning с описанием проблемы -
	 *  тот же принцип "никогда не делать тихо ничего/наполовину", что у
	 *  проверок CellMaterial/CellMesh в GenerateRandom(). */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|Rules")
	void ApplyRuleString();

	/** То же, что ApplyRuleString(), но для HUD: правило приходит параметром
	 *  (из текстового поля виджета, а не из UPROPERTY RuleString), а текст
	 *  ошибки возвращается наружу, чтобы виджет мог показать его пользователю -
	 *  из Blueprint'а warning в логе никак не увидеть, а молча ничего не
	 *  делать в ответ на нажатие кнопки хуже всего.
	 *
	 *  При успехе заодно записывает разобранную строку в RuleString, так что
	 *  Details panel и HUD не расходятся в том, какое правило действует.
	 *  При ошибке не меняет НИЧЕГО (в т.ч. RuleString) - тот же принцип
	 *  "никогда не применять частично", что у ApplyRuleString(). */
	UFUNCTION(BlueprintCallable, Category = "Automata|Rules")
	bool TryApplyRuleString(const FString& InRuleString, FString& OutError);

	/** Действующее правило строкой - собирается из текущих BirthCounts/
	 *  SurvivalCounts/States/Neighborhood (см. RuleStringParser::
	 *  FormatRuleString()), а НЕ читается из UPROPERTY RuleString: то поле
	 *  может быть пустым (правило собрали массивами в Details panel) или
	 *  устаревшим (массивы поправили руками после ApplyRuleString()). HUD
	 *  должен показывать то, по чему реально считается симуляция. */
	UFUNCTION(BlueprintPure, Category = "Automata|Rules")
	FString GetActiveRuleString() const;

	/** Таблица готовых правил для выпадашки в HUD (см. FRulePreset/
	 *  RulePresets::GetAll()). Возвращает копию: 13 маленьких структур,
	 *  вызывается на построение списка, не в горячем цикле. */
	UFUNCTION(BlueprintPure, Category = "Automata|Rules")
	TArray<FRulePreset> GetRulePresets() const;

	/** Применяет пресет по индексу в GetRulePresets(): правило - через тот же
	 *  TryApplyRuleString(), что и ручной ввод (один путь применения правила,
	 *  без второй, отдельно расходящейся ветки), и, если bApplySpawnSettings,
	 *  ещё SpawnRadius/Amount из пресета.
	 *
	 *  Сетку НЕ перегенерирует специально: правило вступает в силу со
	 *  следующего же шага и на текущем состоянии (иногда именно это и нужно -
	 *  посмотреть, что новое правило сделает с уже выросшей структурой), а
	 *  "начать с нуля по пресету" - это отдельное нажатие GenerateRandom()
	 *  следом. Индекс вне диапазона - warning в лог, ничего не меняется. */
	UFUNCTION(BlueprintCallable, Category = "Automata|Rules")
	void ApplyRulePreset(int32 PresetIndex, bool bApplySpawnSettings = true);

	/** Каким методом считается шаг симуляции - CPU (bucket-partitioned
	 *  параллельный алгоритм) или GPU (RDG compute shader, см.
	 *  FGpuComputeStrategy). Пересобирается заново на каждый Next()/
	 *  StepAsync() (см. CreateComputeStrategy()), как и
	 *  FCellularAutomatonRule - правки в Details panel подхватываются
	 *  немедленно. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Rules")
	EComputeMethod ComputeMethod = EComputeMethod::Cpu;

	/** Верхняя граница объёма (в клетках) AABB живых клеток+halo, которую
	 *  FGpuComputeStrategy согласна посчитать на GPU за один Step() - выше
	 *  неё шаг откатывается на CPU с предупреждением в лог (см.
	 *  FGpuComputeStrategy::Step()'s OOM guard). Защита нужна на случай,
	 *  если две живые клетки разлетелись далеко друг от друга в
	 *  разреженной сетке и раздули AABB до неподъёмного объёма, даже если
	 *  самих живых клеток мало. Дефолт 512^3 (~134M клеток, ~16MB буфер на
	 *  вход/выход/readback) - восьмикратный запас над прежним зашитым
	 *  256^3; поднимать дальше можно, если позволяет GPU-память, само
	 *  удаление лимита не рассматривалось (тогда пропадает единственная
	 *  защита от OOM на этом редком сценарии). Передаётся в
	 *  FGpuComputeStrategy через конструктор в CreateComputeStrategy(),
	 *  без кеширования - как и остальные параметры симуляции. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|GPU",
			  meta = (ClampMin = "1"))
	int64 GpuVolumeCellLimit = 512ll * 512ll * 512ll;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
	TSubclassOf<UUserWidget> HUDWidgetClass;
	
	
private:
	TUniquePtr<FUiController> UiController;
	TUniquePtr<FCellGrid> Grid;

	/** Обычный ISMC - существует всегда, независимо от CellMeshComponentType
	 *  (создание/уничтожение компонента в рантейме - лишняя возня и ещё один
	 *  сценарий, который Live Coding не умеет безопасно хот-патчить; оба
	 *  компонента дёшевы, пока в них 0 инстансов). Дочерний относительно
	 *  CellsMeshHierarchical (который root), а не наоборот - см. конструктор. */
	UPROPERTY()
	UInstancedStaticMeshComponent* CellsMeshFlat;

	/** HISM - тоже существует всегда; root component актора (см. конструктор). */
	UPROPERTY()
	UHierarchicalInstancedStaticMeshComponent* CellsMeshHierarchical;

	/** ЕДИНСТВЕННЫЙ рендерер клеток, обёрнутый вокруг
	 *  GetActiveCellsMeshComponent(). Пришёл на смену пулу AgeMeshComponents/
	 *  AgeRenderers (по компоненту на возрастной материал): цвет теперь
	 *  per-instance, поэтому один компонент красит клетки всех возрастов
	 *  сразу - одна бакетизация, один AddInstances(), одно перестроение
	 *  LOD-дерева HISM на поколение вместо N.
	 *  Обычный (не UPROPERTY) член, как SelectionRenderer: переживать
	 *  реинстансинг Live Coding'ом ему не нужно - EnsureCellsRenderer()
	 *  пересоздаёт его по факту рассинхрона с компонентом. */
	TUniquePtr<FInstancedMeshCellGridRenderer> CellsRenderer;

	/** Выделенные мышкой клетки (см. SelectCellsInScreenRect()) - чисто
	 *  рантайм-состояние, не UPROPERTY. Сбрасывается в GenerateRandom()/
	 *  Next()/ApplyStepResult() (после свапа Grid) - смена поколения делает
	 *  старое выделение бессмысленным (рабочий процесс "пауза -> выделение
	 *  -> извлечение", не "выделение во время бесконечного Play"). */
	TArray<FIntVector> SelectedCells;

	/** "Изначальный паттерн" - точка возврата ResetToInitialState() (хоткей
	 *  R) и то, что уходит в файл при Save (см. WriteStateToFile()).
	 *  Заполняется тремя равноправными источниками, каждый раз целиком
	 *  перезаписывая предыдущее значение: StartFromSelection() (Enter -
	 *  извлечённое выделение), LoadStateFromFile() (Ctrl+O - раздел
	 *  InitialCells загруженного файла) и GenerateRandom() (в т.ч. через
	 *  NewSeed() и автоматический вызов из BeginPlay - фактически осевшие
	 *  после генерации клетки, не сырое Amount). В отличие от SelectedCells,
	 *  НЕ сбрасывается ни шагом симуляции (Next()/StepAsync()), ни новым
	 *  выделением мышкой - переживает сколько угодно поколений эволюции.
	 *  Пустой массив означает "ни разу не генерировали/извлекали/грузили в
	 *  этой сессии" - на практике недостижимо в обычном потоке
	 *  использования, так как BeginPlay сам вызывает GenerateRandom(); тогда
	 *  ResetToInitialState() делегирует в GenerateRandom(), а Save
	 *  вежливо отказывает. */
	TArray<FIntVector> InitialStateCells;

	/** Компонент подсветки выделения - всегда обычный (не HISM)
	 *  UInstancedStaticMeshComponent, независимо от CellMeshComponentType:
	 *  выделение всегда маленькое подмножество, LOD-дерево кластеров HISM тут
	 *  не даёт выигрыша. Создаётся лениво (EnsureSelectionMeshComponent()),
	 *  один раз, без пересоздания. UPROPERTY - та же причина, что и у
	 *  GamePC (переживает реинстансинг Live Coding). */
	UPROPERTY(Transient)
	UInstancedStaticMeshComponent* SelectionMeshComponent = nullptr;

	/** Рендерер подсветки поверх SelectionMeshComponent - обычный член (не
	 *  UPROPERTY), как и CellsRenderer. */
	TUniquePtr<FInstancedMeshCellGridRenderer> SelectionRenderer;

	/** Создаёт SelectionMeshComponent/SelectionRenderer при первом
	 *  обращении, если их ещё нет - вызывается из BeginPlay()/OnConstruction()
	 *  и защитно в начале RenderSelectionOverlay(). */
	void EnsureSelectionMeshComponent();

	/** Меш-снимок BakeCellsToMesh() (хоткей M). Создаётся лениво
	 *  (EnsureBakedMeshComponent()), один раз. UPROPERTY - та же причина,
	 *  что у GamePC/SelectionMeshComponent (переживает реинстансинг Live
	 *  Coding). */
	UPROPERTY(Transient)
	UProceduralMeshComponent* BakedMeshComponent = nullptr;

	/** Создаёт BakedMeshComponent при первом обращении - зеркалит
	 *  EnsureSelectionMeshComponent(). */
	void EnsureBakedMeshComponent();

	/** Безусловно обходит ВСЕ реально прикреплённые к актору
	 *  UInstancedStaticMeshComponent (через GetComponents<>(), а не только
	 *  те, что перечислены в CellsMeshFlat/CellsMeshHierarchical/
	 *  SelectionMeshComponent) - вызывается один раз в
	 *  самом начале BeginPlay(), до EnsureCellsRenderer()/
	 *  GenerateRandom(). Компоненты из легитимного набора (объединение
	 *  перечисленных выше) получают ClearInstances(); всё остальное
	 *  уничтожается через DestroyComponent().
	 *
	 *  Зачем полный обход, а не просто ClearInstances() по легитимному
	 *  набору: PIE дублирует актор из редакторского мира вместе с текущим
	 *  СОСТОЯНИЕМ его компонентов - если пользователь успел нажать
	 *  GenerateRandom()/Next() прямо в редакторе перед запуском игры,
	 *  дублируются не только легитимные компоненты с их инстансами, но
	 *  иногда и осиротевшие (не отслеженные ни одним из массивов выше -
	 *  например, оставшиеся после более раннего сбоя реинстансинга Live
	 *  Coding). Такие сироты ClearInstances() по легитимному набору не
	 *  затрагивает - они остаются прикреплены и видимы, накладываясь на
	 *  честно посчитанную симуляцию (мерцание/двоение, которое статичный
	 *  скриншот не всегда ловит). Обнаруженный на практике случай (ещё во
	 *  времена пула AgeMeshComponents): материалов было 3, а на PIE-акторе
	 *  висело 8 InstancedStaticMeshComponent - 5 лишних. Теперь этот же
	 *  механизм заодно подчищает остатки самого пула - после перехода на
	 *  per-instance цвет легитимный набор сократился до трёх компонентов, и
	 *  все рантайм-созданные возрастные компоненты стали сиротами. */
	void ClearAllCellInstances();

	/** Убирает запечённый меш-снимок, если он есть - вызывается в начале
	 *  GenerateRandom()/ResetToInitialState()/StartFromSelection(): новый
	 *  прогон не должен рисоваться сквозь/поверх старого снимка. */
	void ClearBakedMesh();

	/** Грубый "призрачный" силуэт вне ARenderCullVolume (см. bEnableGhostShape) -
	 *  отдельный компонент, НЕ переиспользует BakedMeshComponent: у того
	 *  принципиально другой, взаимоисключающий жизненный цикл ("заморозить и
	 *  выгрузить Grid"), а Ghost Shape наоборот сосуществует с живой
	 *  симуляцией. UPROPERTY - та же причина, что и у остальных Transient-
	 *  компонентов (переживает реинстансинг Live Coding). */
	UPROPERTY(Transient)
	UProceduralMeshComponent* GhostMeshComponent = nullptr;

	/** Создаёт GhostMeshComponent при первом обращении - зеркалит
	 *  EnsureBakedMeshComponent(). */
	void EnsureGhostMeshComponent();

	/** Сколько поколений посчитано с последнего RefreshGhostShape() -
	 *  см. GhostShapeRefreshInterval. Плайн член - пересчитывается заново
	 *  на каждый прогон, переживать реинстансинг незачем. */
	int32 GhostShapeGenerationsSinceRefresh = 0;

	/** Пересчитывает ghost-меш: Grid->GetOccupiedChunkCoords() -> если есть
	 *  активный ARenderCullVolume, фильтр по чанк-AABB против
	 *  GetWorldBounds() (оставляем только чанки СНАРУЖИ куба), иначе -
	 *  ВСЕ занятые чанки целиком (см. doc-comment bEnableGhostShape, режим
	 *  "куба нет") -> CellMeshBuilder::BuildFromCells() через FChunkGridView
	 *  -> CreateMeshSection_LinearColor(). No-op (с ClearGhostShape()), если
	 *  bEnableGhostShape выключен, нет сетки, или (в режиме "снаружи куба")
	 *  снаружи не осталось ни одного занятого чанка. Вызывается из
	 *  ApplyStepResult()/Next() раз в GhostShapeRefreshInterval поколений
	 *  (не каждое - см. её doc-comment), и немедленно из SetGhostShapeEnabled()/
	 *  RefreshRenderCullVolume(). */
	void RefreshGhostShape();

	/** Правда, когда Ghost Shape сейчас покрывает ВСЮ сетку целиком (не
	 *  только снаружи куба) и поэтому должен ЗАМЕНИТЬ детальный поклеточный
	 *  рендер, а не дополнять его - см. doc-comment bEnableGhostShape. Это
	 *  ровно тот случай, когда bEnableGhostShape включён, а активной границы
	 *  отсечения нет (сам куб выключен через bEnableRenderCullVolume, либо
	 *  на уровне вообще нет ARenderCullVolume) - иначе (куб активен) силуэт
	 *  остаётся чистым дополнением снаружи куба, детальный путь работает как
	 *  обычно. Пересчитывается заново на каждый вызов (та же конвенция, что
	 *  у CreateComputeStrategy()/BuildCellRenderData() - никакого кэширования
	 *  между вызовами), используется из RenderGridImmediate()/RenderCurrentGrid(). */
	bool ShouldGhostShapeReplaceDetailedRender();

	/** Убирает ghost-меш, если он есть - вызывается из тех же четырёх точек,
	 *  что и ClearBakedMesh() (GenerateRandom()/StartFromSelection()/
	 *  LoadStateFromFile()/ResetToInitialState()): новый прогон не должен
	 *  показывать силуэт от прошлого. Тоже сбрасывает
	 *  GhostShapeGenerationsSinceRefresh. */
	void ClearGhostShape();

	/** Гарантирует существование Saved/AutomataSaves/ и возвращает её
	 *  абсолютный путь - стартовая папка диалогов Save/Load. */
	FString EnsureSaveDirectory() const;

	/** Собирает JSON-шапку сохранения из текущих UPROPERTY. CellSize берётся
	 *  из Grid->GetCellSize(), а НЕ из UPROPERTY - сетка могла быть создана
	 *  со старым значением, а файл должен фиксировать её фактическую
	 *  геометрию. Добавление нового сохраняемого параметра = одно UPROPERTY в
	 *  FAutomatonSaveHeader + по строке копирования здесь и в
	 *  ApplySaveHeader(). */
	FAutomatonSaveHeader BuildSaveHeader() const;

	/** Применяет параметры из шапки к UPROPERTY - с защитными клампами
	 *  (JSON-шапка правится руками в текстовом редакторе, значениям нельзя
	 *  доверять): CellSize/ChunkSize/Amount/SpawnRadius >= 1,
	 *  ClusterFactor в [0, 1]. Вызывать СТРОГО до CreateGrid() - тот читает
	 *  живые CellSize/ChunkSize. */
	void ApplySaveHeader(const FAutomatonSaveHeader& Header);

	/** Путь последнего успешного сохранения/загрузки в этой сессии - по нему
	 *  SaveState() (Ctrl+S) тихо перезаписывает без диалога. Пусто, пока ни
	 *  разу не сохраняли/загружали. UPROPERTY(Transient) - переживает
	 *  реинстансинг Live Coding (иначе Ctrl+S молча "забыл" бы путь после
	 *  хот-патча и незаметно съехал бы на поведение SaveStateAs()). */
	UPROPERTY(Transient)
	FString LastSaveFilePath;

	/** Общий код "записать InitialStateCells + скриншот текущего вида в
	 *  конкретный FilePath" - без диалога. Используется и SaveState() (путь
	 *  уже известен из LastSaveFilePath), и SaveStateAs() (путь только что
	 *  выбран в диалоге). В файл идёт InitialStateCells (изначальный
	 *  паттерн - тот же набор, что и точка возврата R; заполняется
	 *  StartFromSelection(), LoadStateFromFile() или GenerateRandom() - см.
	 *  её doc-comment) - строится напрямую из этого массива, БЕЗ какого-либо
	 *  обращения к Grid: сетка не сбрасывается, не перерисовывается и никак
	 *  не трогается. Отказ (нет паттерна для сохранения - InitialStateCells
	 *  пуст, на практике недостижимо после первого запуска - см. её
	 *  doc-comment) - причина уже в
	 *  логе, файл не пишется. Миниатюра - CaptureThumbnailPng() снимает
	 *  ровно то, что СЕЙЧАС отрисовано (текущая камера, текущая живая
	 *  симуляция, какой бы она ни была) - не то, что записывается в
	 *  .casave: файл может, например, содержать свежеизвлечённый глайдер, а
	 *  превьюшка - вид на уже проэволюционировавший рой, если пользователь
	 *  успел отлететь и дать симуляции поработать перед нажатием Ctrl+S. При
	 *  успехе обновляет LastSaveFilePath. */
	bool WriteStateToFile(const FString& FilePath);

	/** Снимает скриншот текущего вьюпорта (GEngine->GameViewport->Viewport->
	 *  ReadPixels(), синхронно на игровом потоке, как и весь остальной путь
	 *  сохранения), обрезает его до квадрата ПО ЦЕНТРУ (короткая сторона
	 *  вьюпорта берётся целиком, длинная обрезается симметрично слева/справа
	 *  или сверху/снизу - не искажает пропорции, просто теряет края кадра),
	 *  затем масштабирует до РОВНО ThumbnailSizePixels x ThumbnailSizePixels
	 *  (FImageUtils::ImageResize, безусловно - не только "если больше" -
	 *  иначе размер миниатюры плавал бы вместе с текущим разрешением/
	 *  соотношением сторон окна вместо единого стандартного квадратного
	 *  формата) и кодирует в PNG (FImageUtils::PNGCompressImageArray). Альфа
	 *  бэкбуфера принудительно выставляется в 255 - иначе PNG вышел бы
	 *  прозрачным. Миниатюра - косметика, НЕ часть состояния автомата: любой
	 *  отказ (нет вьюпорта - не PIE; нулевой размер; ReadPixels/кодирование
	 *  не удались) логируется как Warning, OutPngBytes.Reset(), false -
	 *  вызывающая сторона (WriteStateToFile()) обязана продолжить сохранение
	 *  с пустым разделом миниатюры, а не проваливать сохранение целиком. */
	bool CaptureThumbnailPng(TArray64<uint8>& OutPngBytes) const;

	/** Рендерит SelectedCells (отфильтрованные до реально живых - на случай
	 *  рассинхрона) через SelectionRenderer тем же CellMaterial, что и обычные
	 *  клетки, но с цветом SelectionColor в per-instance custom data, одним
	 *  снимком (без чанкинга - выделение всегда маленькое). Не-op, если
	 *  SelectedCells пуст или CellMaterial не назначен. */
	void RenderSelectionOverlay();

	/** Комбинирует NewCells с текущим SelectedCells по CombineMode
	 *  (заменить/добавить/убрать, см. ESelectionCombineMode) - общий код
	 *  прямоугольного выделения (SelectCellsInScreenRect()) и одиночного
	 *  клика (SelectCellUnderCursor()). Только комбинирование - перерисовку
	 *  подсветки и лог делает вызывающая сторона. */
	void CombineWithSelection(TArray<FIntVector>&& NewCells, ESelectionCombineMode CombineMode);

	void InitializeHUD();
	void InitializePlayerController();
	/** Пересчитывает и применяет CellCullStartDistance/CellCullEndDistance
	 *  (или (0, 0), если bEnableCellCulling == false) к CellsMeshFlat/
	 *  CellsMeshHierarchical и SelectionMeshComponent через
	 *  SetCullDistances() - отдельная функция (не встроена в рендер), чтобы
	 *  SetCellCullingEnabled() (хоткей B) могло применить изменение
	 *  немедленно, не дожидаясь следующего рендера нового поколения. */
	void ApplyCellCullDistances();
	/** Создаёт/перепривязывает CellsRenderer на актуальный
	 *  GetActiveCellsMeshComponent(). Одно условие покрывает три случая:
	 *  первый вызов; смена CellMeshComponentType (рендерер обёрнут вокруг
	 *  прошлого компонента - это и показывает GetComponent()); обнуление
	 *  TUniquePtr после реинстансинга Live Coding (сами компоненты - default
	 *  subobject'ы, они выживают, а рендерер нет).
	 *  При перепривязке чистит инстансы у ПРЕДЫДУЩЕГО компонента - иначе
	 *  переключение ISM<->HISM оставило бы старый набор кубиков висеть
	 *  внахлёст с новым. */
	void EnsureCellsRenderer();
	/** Собирает единый список инстансов на отрисовку: живые клетки (цвет по
	 *  AgeColors от Grid->GetAge()) плюс, при States > 2, угасающие (цвет по
	 *  DecayColors/AgeColors от их DecayState) - общий код для
	 *  RenderCurrentGrid() (Play) и RenderGridImmediate() (Next()/
	 *  GenerateRandom()). Пришёл на смену BuildAgeBuckets(): бакетов больше
	 *  нет, цвет уезжает в сам инстанс. Если отсечение активно (см.
	 *  GetActiveCullVolume()) - клетки сперва отсекаются по границам куба
	 *  (GetAliveCellsInBounds()/GetDecayingCellsInBounds()), до построения
	 *  чего бы то ни было. Заполняет LastRenderStats. Не const -
	 *  EnsureRenderCullVolume() лениво кэширует найденный актёр, тот же idiom,
	 *  что EnsureSelectionMeshComponent(). */
	void BuildCellRenderData(TArray<FCellRenderInstance>& OutInstances);
	/** Таблицы цвета на 256 записей: LUT[Age] и LUT[DecayState]. Считаются
	 *  один раз на BuildCellRenderData(), а не на клетку - при миллионах
	 *  клеток интерполяция в цикле это миллионы лишних лерпов, а сама таблица
	 *  занимает 1 КБ. Заодно единственная конверсия в FColor (обязательно
	 *  ToFColor(bSRGB=false), см. FCellRenderInstance) живёт в одном месте. */
	void BuildAgeColorLut(TArray<FColor>& OutLut) const;
	void BuildDecayColorLut(TArray<FColor>& OutLut) const;
	/** Ядро интерполяции: T в [0,1] -> точка на ломаной по ключам Keys.
	 *  Пустой массив -> белый, один ключ -> он же. */
	static FLinearColor SampleColorRamp(const TArray<FLinearColor>& Keys, float T);
	/** Лениво находит и кэширует ARenderCullVolume в мире через
	 *  UGameplayStatics::GetActorOfClass() (тот же идиом, что
	 *  AGamePlayerController использует для поиска САМОГО оркестратора) -
	 *  ревалидирует IsValid() на каждый вызов на случай, если актёр удалён
	 *  в рантайме, и повторно ищет, если кэш пуст/протух. */
	ARenderCullVolume* EnsureRenderCullVolume();
	/** Куб, если он прямо сейчас должен резать клетки, иначе nullptr - т.е.
	 *  единственное место, где живёт условие "отсечение активно": актёр есть
	 *  на уровне, bEnableRenderCullVolume включён, И куб не спрятан
	 *  (ARenderCullVolume::IsVolumeVisible(), хоткей Ctrl+C).
	 *
	 *  Последнее условие добавлено позже и намеренно: сперва видимость и
	 *  отсечение были полностью независимы, но невидимая коробка, продолжающая
	 *  резать, не читается на экране никак - клетки просто обрываются в
	 *  воздухе без видимой причины. Инвариант "спрятан - значит не режет"
	 *  снимает это, не заводя второго состояния: bEnableRenderCullVolume
	 *  никто не перезаписывает, поэтому показать куб обратно (Ctrl+C) - значит
	 *  вернуть отсечение ровно в то состояние, в котором его оставил C, а не
	 *  включить принудительно.
	 *
	 *  Пути, где куб нужен как ПРОСТРАНСТВЕННАЯ ОБЛАСТЬ, а не как отсечение
	 *  рендера (SelectCellsInCullVolume() на L, MoveCullVolumeToSelection()
	 *  на K), сознательно продолжают звать EnsureRenderCullVolume() напрямую -
	 *  область существует независимо от того, режет она сейчас или нет. */
	ARenderCullVolume* GetActiveCullVolume();
	/** Собирает инстансы через BuildCellRenderData() и рендерит их одним
	 *  CellsRenderer - разлитый по кадрам рендер
	 *  (bEnableChunkedRender), если включён. Общий код, вызываемый из
	 *  ApplyStepResult() каждый раз, когда готово новое поколение (в т.ч.
	 *  когда предыдущий чанковый разлив ещё не закончился - см.
	 *  bChunkedRenderInProgress: BeginRender() сам всё сбрасывает и
	 *  перестраивает). */
	void RenderCurrentGrid();
	/** Тот же сбор инстансов, что и в RenderCurrentGrid(), но всегда одним
	 *  снимком (Renderer::Render(), без BeginRender()/чанкинга) - используется
	 *  Next()/GenerateRandom(), которые (как и bEnableChunkedRender/
	 *  StepsPerRender для основного пути) всегда рендерят немедленно и
	 *  целиком, независимо от режима разлитого по кадрам рендера. */
	void RenderGridImmediate();
	/** Возвращает CellsMeshFlat или CellsMeshHierarchical в зависимости от
	 *  CellMeshComponentType - единственное место, которое решает, какой
	 *  компонент сейчас "активен". */
	UInstancedStaticMeshComponent* GetActiveCellsMeshComponent() const;
	/** Чистит инстансы у ТОГО из двух компонентов, который сейчас НЕ выбран
	 *  CellMeshComponentType. Защита от протухшего снимка: активный компонент
	 *  чистит сам BeginRender() каждым рендером, а вот на неактивном инстансы
	 *  могут остаться после переключения типа или прийти из уровня,
	 *  сохранённого до перехода на per-instance цвет - и тогда две сетки
	 *  видны внахлёст. Чистить здесь активный (как делала прежняя версия)
	 *  нельзя: теперь рисуем именно в него, и рендер обнулял бы сам себя. */
	void ClearInactiveCellsMeshComponent();
	/** Строит новую пустую сетку по текущим CellSize/ChunkSize из Details
	 *  panel. Используется и GenerateRandom() (сетка с нуля), и Next()
	 *  (буфер для следующего поколения). */
	TUniquePtr<FCellGrid> CreateGrid() const;

	/** Строит новую стратегию расчёта шага по текущему ComputeMethod из
	 *  Details panel - зеркалит CreateGrid()'s switch-паттерн. Используется
	 *  и Next(), и StepAsync(). */
	TUniquePtr<FCellularAutomatonComputeStrategy> CreateComputeStrategy() const;

	/** UPROPERTY (не голый указатель) - иначе после реинстансинга через
	 *  Live Coding во время активного PIE значение не переживает пересборку
	 *  класса (BeginPlay(), который его заполняет, повторно не вызывается) и
	 *  остаётся мусором, а не nullptr - разыменование такого указателя
	 *  роняет редактор Access Violation. */
	UPROPERTY(Transient)
	AGamePlayerController* GamePC = nullptr;

	/** Кэш EnsureRenderCullVolume() - UPROPERTY(Transient) по той же
	 *  причине, что GamePC выше (переживает реинстансинг Live Coding, не
	 *  остаётся мусором). */
	UPROPERTY(Transient)
	ARenderCullVolume* CachedRenderCullVolume = nullptr;

	/** Положение и направление камеры на момент последнего построения среза -
	 *  с ними сравниваются текущие, чтобы решить, пора ли перестраивать (см.
	 *  ViewSliceCameraMoveThreshold). Плайн члены, не UPROPERTY: после
	 *  реинстансинга Live Coding они обнулятся, и худшее, что случится, -
	 *  один лишний пере-рендер. */
	FVector LastViewSliceCameraLocation = FVector::ZeroVector;
	FVector LastViewSliceCameraForward = FVector::ZeroVector;
	bool bHasViewSliceCameraState = false;

	/** Текущие положение/направление камеры; false, если камеры ещё нет
	 *  (до BeginPlay/вне PIE). */
	bool GetCameraView(FVector& OutLocation, FVector& OutForward) const;

	/** Показать сообщение поверх картинки на несколько секунд. Нужно там, где
	 *  хоткей меняет что-то, чего на экране может быть сразу и не видно: строка
	 *  в логе во время PIE не видна вовсе, и понять, дошло ли нажатие,
	 *  нельзя - ровно эта жалоба и была про срез вдоль взгляда.
	 *
	 *  Key - постоянный идентификатор сообщения, а не -1: хоткеи [ и ]
	 *  привязаны к Triggered и на удержании срабатывают каждый кадр, так что
	 *  с -1 экран за секунду забился бы сотней строк. С постоянным ключом
	 *  новое сообщение заменяет предыдущее на месте. Значения ключей
	 *  перечислены в EStatusMessageKey. */
	void ShowStatusMessage(int32 Key, const FString& Message) const;

	/** Идентификаторы сообщений для ShowStatusMessage() - каждому виду свой,
	 *  чтобы повторные нажатия заменяли строку, а разные сообщения не
	 *  затирали друг друга. */
	enum EStatusMessageKey : int32
	{
		StatusKey_ViewSlice = 1001,
	};

	/** Сдвинулась ли камера настолько, что срез пора перестроить. Всегда
	 *  false, если срез выключен. */
	bool ShouldRefreshViewSlice() const;

	/** См. GetLastRenderStats()/FCellRenderStats - плайн член (не
	 *  UPROPERTY), заполняется заново в каждом BuildCellRenderData(), не нужно
	 *  переживать реинстансинг Live Coding (просто пересчитается на
	 *  следующем рендере). */
	FCellRenderStats LastRenderStats;

	/** См. GetHudStats()/FHudStats. UPROPERTY (не плайн член, в отличие от
	 *  LastRenderStats выше) - FHudStats это USTRUCT, а её GenerationCount/
	 *  GenerationsPerSecond должны переживать реинстансинг Live Coding
	 *  между кадрами (иначе, в отличие от LastRenderStats, они не
	 *  пересчитываются каждый рендер - только раз в секунду и на шаге). */
	UPROPERTY(Transient)
	FHudStats LastHudStats;

	/** Байты последнего GPU-compute входного буфера (см.
	 *  FHudStats::EstimatedGpuComputeUploadMB) - обновляется в
	 *  ApplyStepResult() (Play/автошаг) и в завершении Next() (ручной шаг),
	 *  читается в Tick() в LastHudStats. 0, если последний шаг считался на
	 *  CPU или сетка ещё не запускалась. */
	int64 LastGpuComputeUploadBytes = 0;

	/** Сквозной счётчик поколений с последнего GenerateRandom()/
	 *  ResetToInitialState() - в отличие от StepsSinceLastRender (сбрасывается
	 *  на каждом рендере) этот только растёт, пока не начат новый прогон.
	 *  Инкрементируется в завершении ApplyStepResult() (путь Play/автошаг) и
	 *  в завершении Next() (ручной шаг) - в обоих местах ровно по одному
	 *  разу за реально посчитанное поколение (Next() сам крутит NumSteps
	 *  поколений за одно нажатие - см. её тело - поэтому там инкремент на
	 *  NumSteps, а не на 1). */
	int64 GenerationCount = 0;

	/** Для UpdateGenerationsPerSecond() (см. Tick()) - снимок GenerationCount/
	 *  времени на момент последнего пересчёта частоты, не каждый кадр. */
	int64 LastGenerationCountSample = 0;
	double LastGenerationCountSampleSeconds = 0.0;

	/** Раз в секунду (не каждый кадр) пересчитывает LastHudStats.
	 *  GenerationsPerSecond из GenerationCount - вызывается из UpdateHudStats(). */
	void UpdateGenerationsPerSecond();

	/** Пересобирает LastHudStats из текущего состояния. Зовётся и из Tick()
	 *  (пока симуляция идёт), и из самого GetHudStats() - см. его doc-comment
	 *  за тем, почему одного Tick() недостаточно. */
	void UpdateHudStats();

	/** Сбрасывает GenerationCount и точку отсчёта GenerationsPerSecond в 0 -
	 *  общий код для всех мест, начинающих новый прогон "с нуля" (GenerateRandom()/
	 *  StartFromSelection()/LoadStateFromFile()/ResetToInitialState() - те же
	 *  четыре места, что перезаписывают InitialStateCells, см. её doc-comment). */
	void ResetGenerationCounter();

	/** true между Start() и Stop() - Tick() копит DeltaTime и вызывает
	 *  StepAsync() с интервалом 1/Speed секунд, пока флаг не сброшен. */
	bool bSimulationRunning = false;
	float TimeSinceLastStep = 0.0f;

	/** true между StartFastStep() и StopFastStep() (Shift+F, пока зажато) -
	 *  Tick() шагает в этой ветке тем же темпом по Speed и тем же
	 *  TimeSinceLastStep, что и bSimulationRunning (взаимоисключающи,
	 *  делить аккумулятор безопасно). Обычное F без Shift сюда не попадает -
	 *  оно вызывает Next() один раз напрямую, минуя это состояние. */
	bool bFastStepActive = false;

	/** true с момента запуска фонового шага (StepAsync()) и до момента, когда
	 *  ApplyStepResult() переподставляет Grid новым поколением (сбрасывается
	 *  в самом начале ApplyStepResult(), а не в конце) - защищает только сам
	 *  Grid-указатель на время фонового чтения, а не последующий рендер/
	 *  чанковый "разлив" (тот гейтится только своим собственным
	 *  bChunkedRenderInProgress, и то лишь чтобы знать, нужно ли его прервать -
	 *  не чтобы дождаться). Пока true: Tick() не запускает следующий шаг
	 *  поверх текущего (гонка на Grid), а Next()/GenerateRandom() отказываются
	 *  выполняться (та же причина - фоновый поток в это время читает *Grid). */
	bool bStepInProgress = false;

	/** R (ResetToInitialState()), нажатый, пока bStepInProgress ещё true -
	 *  раньше ResetToInitialState() в этом случае просто отказывалась
	 *  (warning в лог) и ничего больше не делала, так что нажатие R молча
	 *  терялось, если оно случайно совпало с фоновым шагом, а симуляция
	 *  просто продолжала идти дальше - именно это пользователь наблюдал как
	 *  "R не всегда срабатывает" (отдельно от более раннего фикса
	 *  сэмплирования Enhanced Input в AGamePlayerController::InputKey() -
	 *  оба бага давали один и тот же на вид симптом, но это два разных
	 *  места). Теперь ResetToInitialState() вместо простого отказа
	 *  выставляет этот флаг и возвращается - ApplyStepResult() и завершение
	 *  фоновой лямбды в Next() оба проверяют его сразу после того, как сами
	 *  сбрасывают bStepInProgress в false, и если он взведён, немедленно
	 *  зовут ResetToInitialState() вместо обычного применения только что
	 *  посчитанного поколения (которое всё равно тут же было бы
	 *  перезаписано сбросом). */
	bool bResetToInitialStatePending = false;

	/** N (NewSeed()), нажатая, пока bStepInProgress ещё true - ровно тот же
	 *  баг и то же решение, что у bResetToInitialStatePending выше: реролл
	 *  идёт через GenerateRandom(), который отказывается работать во время
	 *  фонового шага, так что нажатие, совпавшее с ним, молча терялось
	 *  ("срабатывает только в определённые моменты"). Взводится вместо отказа
	 *  и разряжается там же, где отложенный сброс - в ApplyStepResult() и в
	 *  завершении фоновой лямбды Next(), сразу после сброса bStepInProgress.
	 *  Взаимоисключающ с bResetToInitialStatePending: N и R хотят двух
	 *  несовместимых вещей от одного и того же Grid (новый случайный узор
	 *  против ровно того же исходного), поэтому каждый из них гасит флаг
	 *  другого - выигрывает последнее нажатие, а не то, которое случайно
	 *  проверяется первым. */
	bool bNewSeedPending = false;

	/** Сколько поколений посчитано с последнего фактического рендера -
	 *  сбрасывается в 0 сразу после реального рендера (ApplyStepResult()/
	 *  AdvanceChunkedRender()), и в GenerateRandom()/Start()/StartFastStep()
	 *  (свежий прогон считает с нуля). См. StepsPerRender. */
	int32 StepsSinceLastRender = 0;

	/** Сколько поколений ФАКТИЧЕСКИ посчитал последний фоновый заход: 1 -
	 *  обычный ритм "поколение за заход", больше - пачка целиком внутри одного
	 *  круга через GPU. Пишется в ApplyStepResult() по факту, а не в
	 *  StepAsync() по плану - пачка может быть урезана уже внутри стратегии
	 *  (объём AABB упёрся в её потолок), и планируемое число разошлось бы с
	 *  реальным. Читается только Tick()'ом, чтобы держать интервал между
	 *  заходами равным (поколений за заход)/Speed - иначе пачка молча умножила
	 *  бы фактическую частоту поколений на StepsPerRender, а урезанная пачка
	 *  во столько же раз её замедлила. Обычный член, не UPROPERTY: чистое
	 *  рантайм-состояние темпа, переживать Live Coding ему незачем (сбросится
	 *  в 1 и сойдётся обратно на первом же заходе). */
	int32 LastDispatchGenerations = 1;

	/** Асинхронная версия шага симуляции для непрерывного Play (используется
	 *  только из Tick()) - тяжёлый FCellularAutomatonRule::Step() считается в
	 *  фоновом потоке (EAsyncExecution::ThreadPool), чтобы не блокировать
	 *  game thread (а с ним и камеру/интерфейс) на время шага. Rule и
	 *  next-gen буфер строятся на game thread ДО диспетчеризации (снимок
	 *  текущих Details-panel значений), чтобы фоновый поток не читал
	 *  UPROPERTY одновременно с возможной правкой в редакторе. Next()
	 *  (ручная кнопка) остаётся синхронной - для непрерывной игры не
	 *  используется. */
	void StepAsync();

	/** Завершение StepAsync() - выполняется на game thread через
	 *  AsyncTask(ENamedThreads::GameThread, ...), т.к. UInstancedStaticMeshComponent
	 *  (и HISM) не потокобезопасны. Подставляет посчитанный NewGrid и рендерит его.
	 *  ComputeUploadBytes - см. LastGpuComputeUploadBytes, снят с ComputeStrategy
	 *  ещё в фоновом потоке (см. FCellularAutomatonComputeStrategy::
	 *  GetLastComputeUploadBytes()), до того как сама стратегия будет
	 *  уничтожена по завершении фоновой лямбды. */
	void ApplyStepResult(TUniquePtr<FCellGrid> NewGrid, double StepSeconds, int64 ComputeUploadBytes, int32 GenerationsAdvanced);

	/** Future от Async() в StepAsync() - StepAsync() передаёт фоновому потоку
	 *  сырой Grid.Get() (см. StepAsync()), поэтому EndPlay() обязан дождаться
	 *  этого future перед тем, как актор (а с ним и Grid) начнёт разрушаться -
	 *  иначе фоновый поток может разыменовать уже освобождённый Grid
	 *  (Access Violation внутри FCellularAutomatonRule::Step()'s ParallelFor).
	 *  Всегда максимум один шаг в полёте одновременно (см. bStepInProgress),
	 *  так что перезапись этого поля на каждый StepAsync() безопасна. */
	TFuture<void> PendingStepFuture;

	/** true, пока рендер текущего поколения "разлит" по кадрам (см.
	 *  bEnableChunkedRender) - Tick() вызывает AdvanceChunkedRender()
	 *  каждый кадр, пока флаг не сброшен. bStepInProgress уже false к этому
	 *  моменту (см. его комментарий) - фоновый счёт следующих поколений
	 *  продолжается параллельно с этим "разливом" (см. StepsPerRender), а не
	 *  ждёт его окончания. Если новое поколение досчитается раньше, чем этот
	 *  разлив закончится, ApplyStepResult() прерывает его немедленно
	 *  (RenderCurrentGrid() -> BeginRender() сбрасывает и перестраивает всё
	 *  с нуля), вместо того чтобы ждать его завершения. */
	bool bChunkedRenderInProgress = false;
	double ChunkedRenderStartSeconds = 0.0;
	int32 ChunkedRenderFrameCount = 0;

	/** Добавляет очередную порцию инстансов в CellsRenderer
	 *  (AdvanceRenderChunk() с полным бюджетом ChunkedRenderCellsPerFrame -
	 *  прежнее деление бюджета между возрастными бакетами исчезло вместе с
	 *  ними, так что при N материалах кадр стал в ~N раз тяжелее и разлив во
	 *  столько же раз короче; подобранное раньше значение стоит перепроверить)
	 *  и, когда рендерер не вернул "ещё есть, что дорисовать", сбрасывает
	 *  bChunkedRenderInProgress и логирует итог (сколько кадров/времени заняло). */
	void AdvanceChunkedRender();

	/** Досыпает все оставшиеся инстансы чанкового рендера одним вызовом
	 *  (AdvanceRenderChunk(TNumericLimits<int32>::Max()))
	 *  вместо того, чтобы ждать, пока AdvanceChunkedRender() доедет по кадрам -
	 *  вызывается из Stop() (P), чтобы остановка не оставляла сетку висеть
	 *  недорисованной. Не-op, если чанковый рендер сейчас не идёт. */
	void FinishChunkedRenderImmediately();
};