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
#include "Automata/Simulation/Neighborhood.h"
#include "GameFramework/PlayerController.h"
#include "AutomataOrchestrator.generated.h"

class UInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class FCellularAutomatonComputeStrategy;

/** Метод расчёта шага симуляции. Gpu пока заглушка (см.
 *  FGpuComputeStrategy) - делегирует на CPU-алгоритм с предупреждением в
 *  лог, пока не появится реальный compute-shader бэкенд. */
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
	virtual void PostActorCreated() override;
#if WITH_EDITOR
	/** Правки AgeMaterials в Details panel (добавили/убрали элемент) должны
	 *  сразу же создать/удалить соответствующие AgeMeshComponents - не
	 *  дожидаясь следующего рендера. */
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

	/** Выполнить один шаг симуляции */
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

	/** Выбирает живые клетки, чья экранная проекция попадает в прямоугольник
	 *  [RectMin, RectMax] (без ограничения по глубине - см. CellSelection::
	 *  SelectCellsInScreenRect()), кладёт результат в SelectedCells и сразу
	 *  перерисовывает подсветку (RenderSelectionOverlay()), не дожидаясь
	 *  следующего шага симуляции. Матрицу вида-проекции строит вызывающий код
	 *  (AGamePlayerController::OnSelectDragFinished()) один раз на всю
	 *  операцию, не на клетку. */
	UFUNCTION(BlueprintCallable, Category = "Automata|Selection")
	void SelectCellsInScreenRect(const FMatrix& ViewProjectionMatrix, const FVector2D& ViewportSize, const FVector2D& RectMin, const FVector2D& RectMax);

	/** Делает клетки из SelectedCells единственным содержимым новой сетки
	 *  (возраст сброшен - как только что родившиеся) и выходит из режима
	 *  выделения. Мировые координаты НЕ переносятся к началу координат -
	 *  клетки остаются там же, где их выделили (правила автомата
	 *  трансляционно инвариантны, а камера и так уже смотрит именно туда).
	 *  Отказывает (с warning в лог), если ничего не выделено - тот же паттерн
	 *  guard'ов, что и у остальных методов этого класса. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Automata|Selection")
	void StartFromSelection();

	/** Материал подсветки выделенных клеток - отдельный рендер-проход поверх
	 *  обычного возрастного рендера (см. SelectionMeshComponent/
	 *  SelectionRenderer), рисуется тем же CellMesh, что и обычные клетки. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Selection")
	UMaterialInterface* SelectionMaterial = nullptr;

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

	/** Материалы клеток по возрасту (сколько поколений подряд клетка прожила) -
	 *  от самых старых (первый элемент) к самым молодым (последний) - ЕДИНСТВЕННЫЙ
	 *  источник материала для клеток в этом проекте (раньше был отдельный
	 *  CellMaterial "по умолчанию, без возраста" - убран: раз рендер всё равно
	 *  всегда идёт через возрастные бакеты, отдельный материал-заглушка только
	 *  усложнял бы конфигурацию и провоцировал именно ту путаницу, из-за которой
	 *  его убрали - "почему показывается не то" при пустом/неполном массиве).
	 *  ОБЯЗАТЕЛЕН - должен содержать хотя бы 1 элемент, иначе GenerateRandom()/
	 *  Next()/StepAsync() откажутся выполняться (с warning в лог), как и при
	 *  отсутствии CellMesh. Клетка возраста 0 (только родилась) красится
	 *  ПОСЛЕДНИМ материалом массива, клетка возраста (N-1) и старше - ПЕРВЫМ;
	 *  формула MaterialIndex = N-1-min(Age, N-1) (см. BuildAgeBuckets()). При
	 *  N==1 все клетки красятся этим единственным материалом независимо от
	 *  возраста - эквивалент старого поведения с одним CellMaterial. Материал в
	 *  UInstancedStaticMeshComponent/HISM задаётся на весь компонент целиком, не
	 *  на инстанс - поэтому N разных материалов реализованы N отдельными,
	 *  создаваемыми в рантайме компонентами (см. AgeMeshComponents/
	 *  RebuildAgeMeshComponents()), а не одним. Учитывается во всех путях
	 *  рендера одинаково - и в непрерывной симуляции (Play/StepAsync ->
	 *  RenderCurrentGrid(), чанково), и в ручном шаге/спавне (Next()/
	 *  GenerateRandom() -> RenderGridImmediate(), одним снимком). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	TArray<UMaterialInterface*> AgeMaterials;

	/** Размер одной клетки в мировых единицах */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "1.0", UIMin = "1.0", UIMax = "1000.0"))
	float CellSize = 100.0f;

	/** Какая реализация инстансированного компонента используется для
	 *  отрисовки клеток - выбирает класс (ISM/HISM) как для каждого
	 *  компонента в AgeMeshComponents (см. RebuildAgeMeshComponents()), так и
	 *  для CellsMeshFlat/CellsMeshHierarchical (GetActiveCellsMeshComponent(),
	 *  оставлены как постоянный root/attach-parent - см. их doc-comment).
	 *  Правка пересоздаёт пул AgeMeshComponents под новый класс (см.
	 *  RebuildAgeMeshComponents()), а не переключает существующие компоненты. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	ECellMeshComponentType CellMeshComponentType = ECellMeshComponentType::HierarchicalInstanced;

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
	 *  применяется к CellsMeshFlat/CellsMeshHierarchical и ко всем
	 *  AgeMeshComponents в ApplyCellCullDistances() на каждый рендер (сама
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
	 *  кадрам рендер. Актуально только при bEnableChunkedRender == true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "1", EditCondition = "bEnableChunkedRender", EditConditionHides))
	int32 ChunkedRenderCellsPerFrame = 20000;

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
			  meta = (ClampMin = "1"))
	int32 StepsPerRender = 1;

	/** Меняет StepsPerRender на Delta (хоткеи [ и ] в AGamePlayerController),
	 *  клампится снизу к 1 - StepsPerRender не может быть меньше 1 (рендерить
	 *  реже раза в поколение не имеет смысла). */
	UFUNCTION(BlueprintCallable, Category = "Automata")
	void AdjustStepsPerRender(int32 Delta);

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

	/** Пул компонентов для рендера по возрасту (см. AgeMaterials) - ровно один
	 *  компонент на элемент AgeMaterials, в отличие от CellsMeshFlat/
	 *  CellsMeshHierarchical создаются/уничтожаются в рантайме через
	 *  NewObject()+RegisterComponent()/DestroyComponent() по мере правки
	 *  размера AgeMaterials (см. RebuildAgeMeshComponents()) - это не тот же
	 *  случай, что уже кусал проект со сменой класса CreateDefaultSubobject-
	 *  компонента (см. CLAUDE.md): тут компоненты не default subobject'ы, и
	 *  уничтожать/пересоздавать их в рантайме - штатная операция. UPROPERTY
	 *  (не голый массив) по той же причине, что и GamePC - иначе после
	 *  реинстансинга Live Coding'ом массив останется валиден, а вот
	 *  параллельный AgeRenderers (не UPROPERTY, см. ниже) обнулится, и их
	 *  придётся пересинхронизировать (см. RebuildAgeMeshComponents()). */
	UPROPERTY(Transient)
	TArray<UInstancedStaticMeshComponent*> AgeMeshComponents;

	/** Один FInstancedMeshCellGridRenderer на элемент AgeMeshComponents/
	 *  AgeMaterials, тем же индексом. Обычный (не UPROPERTY) член - см.
	 *  AgeMeshComponents выше про рассинхронизацию после Live Coding. */
	TArray<TUniquePtr<FInstancedMeshCellGridRenderer>> AgeRenderers;

	/** Выделенные мышкой клетки (см. SelectCellsInScreenRect()) - чисто
	 *  рантайм-состояние, не UPROPERTY. Сбрасывается в GenerateRandom()/
	 *  Next()/ApplyStepResult() (после свапа Grid) - смена поколения делает
	 *  старое выделение бессмысленным (рабочий процесс "пауза -> выделение
	 *  -> извлечение", не "выделение во время бесконечного Play"). */
	TArray<FIntVector> SelectedCells;

	/** Компонент подсветки выделения - всегда обычный (не HISM)
	 *  UInstancedStaticMeshComponent, независимо от CellMeshComponentType:
	 *  выделение всегда маленькое подмножество, LOD-дерево кластеров HISM тут
	 *  не даёт выигрыша. Создаётся лениво (EnsureSelectionMeshComponent()),
	 *  один раз, без пересоздания. UPROPERTY - та же причина, что и у
	 *  GamePC/AgeMeshComponents (переживает реинстансинг Live Coding). */
	UPROPERTY(Transient)
	UInstancedStaticMeshComponent* SelectionMeshComponent = nullptr;

	/** Рендерер подсветки поверх SelectionMeshComponent - обычный член (не
	 *  UPROPERTY), как и AgeRenderers. */
	TUniquePtr<FInstancedMeshCellGridRenderer> SelectionRenderer;

	/** Создаёт SelectionMeshComponent/SelectionRenderer при первом
	 *  обращении, если их ещё нет - вызывается из BeginPlay()/OnConstruction()
	 *  и защитно в начале RenderSelectionOverlay(). */
	void EnsureSelectionMeshComponent();

	/** Рендерит SelectedCells (отфильтрованные до реально живых - на случай
	 *  рассинхрона) через SelectionRenderer с материалом SelectionMaterial,
	 *  одним снимком (без чанкинга - выделение всегда маленькое). Не-op, если
	 *  SelectedCells пуст или SelectionMaterial не назначен. */
	void RenderSelectionOverlay();

	void InitializeHUD();
	void InitializePlayerController();
	/** Пересчитывает и применяет CellCullStartDistance/CellCullEndDistance
	 *  (или (0, 0), если bEnableCellCulling == false) к CellsMeshFlat/
	 *  CellsMeshHierarchical и ко всем AgeMeshComponents через
	 *  SetCullDistances() - отдельная функция (не встроена в рендер), чтобы
	 *  SetCellCullingEnabled() (хоткей B) могло применить изменение
	 *  немедленно, не дожидаясь следующего рендера нового поколения. */
	void ApplyCellCullDistances();
	/** Пересоздаёт AgeMeshComponents/AgeRenderers так, чтобы их было ровно
	 *  AgeMaterials.Num() штук нужного (текущий CellMeshComponentType, ISM
	 *  или HISM) класса - вызывается из RenderCurrentGrid()/RenderGridImmediate(),
	 *  BeginPlay()/OnConstruction() и PostEditChangeProperty(). Дёшево звать
	 *  повторно без изменений (оба while-цикла роста/сокращения сразу
	 *  становятся no-op, если размер уже совпадает); также пересинхронизирует
	 *  AgeRenderers с AgeMeshComponents, если они разошлись по количеству
	 *  (см. doc-comment AgeMeshComponents про реинстансинг Live Coding). */
	void RebuildAgeMeshComponents();
	/** Раскладывает Grid->GetAliveCells() на AgeMaterials.Num() бакетов по
	 *  MaterialIndex = N-1-min(Age, N-1) (см. doc-comment AgeMaterials) -
	 *  общий код для RenderCurrentGrid() (Play) и RenderGridImmediate()
	 *  (Next()/GenerateRandom()), чтобы не дублировать сам цикл бакетирования. */
	TArray<TArray<FIntVector>> BuildAgeBuckets() const;
	/** Раскладывает живые клетки по AgeMaterials.Num() бакетам по возрасту
	 *  (см. AgeMaterials/BuildAgeBuckets()) и рендерит каждый бакет через свой
	 *  AgeRenderers[i] на AgeMeshComponents[i] - разлитый по кадрам рендер
	 *  (bEnableChunkedRender), если включён. Общий код, вызываемый из
	 *  ApplyStepResult() каждый раз, когда готово новое поколение (в т.ч.
	 *  когда предыдущий чанковый разлив ещё не закончился - см.
	 *  bChunkedRenderInProgress: BeginRender() сам всё сбрасывает и
	 *  перестраивает). */
	void RenderCurrentGrid();
	/** То же бакетирование, что и RenderCurrentGrid(), но всегда одним
	 *  снимком (Renderer::Render(), без BeginRender()/чанкинга) - используется
	 *  Next()/GenerateRandom(), которые (как и bEnableChunkedRender/
	 *  StepsPerRender для основного пути) всегда рендерят немедленно и
	 *  целиком, независимо от режима разлитого по кадрам рендера. */
	void RenderGridImmediate();
	/** Возвращает CellsMeshFlat или CellsMeshHierarchical в зависимости от
	 *  CellMeshComponentType - единственное место, которое решает, какой
	 *  компонент сейчас "активен". */
	UInstancedStaticMeshComponent* GetActiveCellsMeshComponent() const;
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

	/** Сколько поколений посчитано с последнего фактического рендера -
	 *  сбрасывается в 0 сразу после реального рендера (ApplyStepResult()/
	 *  AdvanceChunkedRender()), и в GenerateRandom()/Start()/StartFastStep()
	 *  (свежий прогон считает с нуля). См. StepsPerRender. */
	int32 StepsSinceLastRender = 0;

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
	 *  (и HISM) не потокобезопасны. Подставляет посчитанный NewGrid и рендерит его. */
	void ApplyStepResult(TUniquePtr<FCellGrid> NewGrid, double StepSeconds);

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

	/** Добавляет очередную порцию инстансов на каждый AgeRenderers[i]
	 *  (AdvanceRenderChunk(), бюджет ChunkedRenderCellsPerFrame поровну между
	 *  бакетами - см. doc-comment реализации) и, когда ни один из них не
	 *  вернул "ещё есть, что дорисовать", сбрасывает bChunkedRenderInProgress
	 *  и логирует итог (сколько кадров/времени заняло). */
	void AdvanceChunkedRender();

	/** Досыпает все оставшиеся инстансы чанкового рендера у каждого
	 *  AgeRenderers[i] одним вызовом (AdvanceRenderChunk(TNumericLimits<int32>::Max()))
	 *  вместо того, чтобы ждать, пока AdvanceChunkedRender() доедет по кадрам -
	 *  вызывается из Stop() (P), чтобы остановка не оставляла сетку висеть
	 *  недорисованной. Не-op, если чанковый рендер сейчас не идёт. */
	void FinishChunkedRenderImmediately();
};