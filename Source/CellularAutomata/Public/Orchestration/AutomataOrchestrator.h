// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Blueprint/UserWidget.h"
#include "CellularAutomata/Public/Core/PlayerController/GamePlayerController.h"
#include "CellularAutomata/Public/Ui/UiController.h"
#include "Automata/Grid/CellGrid.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "Automata/Simulation/Neighborhood.h"
#include "GameFramework/PlayerController.h"
#include "AutomataOrchestrator.generated.h"

class UInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;

/** Стратегия хранения клеток сетки автомата. */
UENUM(BlueprintType)
enum class EGridStorageStrategy : uint8
{
	Sparse,
	Dense
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

	/** Стратегия хранения живых клеток: Sparse (TSet, эффективна при
	 *  малой плотности) или Dense (чанками, эффективна при высокой
	 *  плотности / больших локальных скоплениях клеток) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Grid")
	EGridStorageStrategy GridStorageStrategy = EGridStorageStrategy::Sparse;

	/** Размер стороны чанка (в клетках) для Dense-сетки: чанк хранит
	 *  ChunkSize^3 клеток как плотный битовый массив. Актуально только
	 *  при GridStorageStrategy == Dense. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Grid",
			  meta = (ClampMin = "1", UIMin = "4", UIMax = "64",
					  EditCondition = "GridStorageStrategy == EGridStorageStrategy::Dense",
					  EditConditionHides))
	int32 ChunkSize = 16;

	/** Меш, используемый для отрисовки одной клетки автомата (инстансированный) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	UStaticMesh* CellMesh = nullptr;

	/** Материал для клеток (опционально - переопределяет материал меша) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	UMaterialInterface* CellMaterial = nullptr;

	/** Размер одной клетки в мировых единицах */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells",
			  meta = (ClampMin = "1.0", UIMin = "1.0", UIMax = "1000.0"))
	float CellSize = 100.0f;

	/** Какая реализация инстансированного компонента реально используется
	 *  для отрисовки - переключается лениво, на следующем Render() (см.
	 *  InitializeRenderer()/GetActiveCellsMeshComponent()), без пересборки
	 *  C++ и без пересоздания компонентов: оба (CellsMeshFlat/
	 *  CellsMeshHierarchical) существуют постоянно, выбирается только,
	 *  который из них получает инстансы. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	ECellMeshComponentType CellMeshComponentType = ECellMeshComponentType::HierarchicalInstanced;

	/** Включает/выключает разлитый по кадрам рендер целиком (см.
	 *  ChunkedRenderCellsPerFrame) - если false, StepAsync() всегда рендерит
	 *  новую сетку одним кадром, как до появления чанкинга. Никакого
	 *  автоматического порога по числу клеток нет - переключается только
	 *  вручную (Details panel или хоткей Z, см.
	 *  AGamePlayerController::OnToggleChunkedRender()). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	bool bEnableChunkedRender = true;

	/** Включено ли сейчас разлитое по кадрам рендер - нужно внешнему коду
	 *  (хоткею C), чтобы решить, на что переключать, не трогая
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
	TSubclassOf<UUserWidget> HUDWidgetClass;
	
	
private:
	TUniquePtr<FUiController> UiController;
	TUniquePtr<FCellGrid> Grid;
	TUniquePtr<FInstancedMeshCellGridRenderer> Renderer;

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

	void InitializeHUD();
	void InitializePlayerController();
	void InitializeRenderer();
	/** Возвращает CellsMeshFlat или CellsMeshHierarchical в зависимости от
	 *  CellMeshComponentType - единственное место, которое решает, какой
	 *  компонент сейчас "активен". */
	UInstancedStaticMeshComponent* GetActiveCellsMeshComponent() const;
	/** Строит новую пустую сетку по текущим GridStorageStrategy/CellSize/
	 *  ChunkSize из Details panel. Используется и GenerateRandom() (сетка
	 *  с нуля), и Next() (буфер для следующего поколения). */
	TUniquePtr<FCellGrid> CreateGrid() const;

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

	/** true с момента запуска фонового шага (StepAsync()) и до применения
	 *  его результата на game thread (ApplyStepResult()). Пока true: Tick()
	 *  не запускает следующий шаг поверх текущего (гонка на Grid), а
	 *  Next()/GenerateRandom() отказываются выполняться (та же причина -
	 *  фоновый поток в это время читает *Grid). */
	bool bStepInProgress = false;

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
	 *  каждый кадр, пока флаг не сброшен. bStepInProgress остаётся true всё
	 *  это время (переиспользуем как единый guard "занят предыдущим
	 *  поколением"), поэтому следующий StepAsync()/ручной Next()/
	 *  GenerateRandom() не стартуют поверх незаконченного рендера. */
	bool bChunkedRenderInProgress = false;
	double ChunkedRenderStartSeconds = 0.0;
	int32 ChunkedRenderFrameCount = 0;

	/** Добавляет очередную порцию инстансов (см. Renderer::AdvanceRenderChunk())
	 *  и, когда рендер полностью завершён, сбрасывает bChunkedRenderInProgress/
	 *  bStepInProgress и логирует итог (сколько кадров/времени заняло). */
	void AdvanceChunkedRender();

	/** Досыпает все оставшиеся инстансы чанкового рендера одним вызовом
	 *  (Renderer::AdvanceRenderChunk(TNumericLimits<int32>::Max())) вместо
	 *  того, чтобы ждать, пока AdvanceChunkedRender() доедет по кадрам -
	 *  вызывается из Stop() (P), чтобы остановка не оставляла сетку висеть
	 *  недорисованной. Не-op, если чанковый рендер сейчас не идёт. */
	void FinishChunkedRenderImmediately();
};