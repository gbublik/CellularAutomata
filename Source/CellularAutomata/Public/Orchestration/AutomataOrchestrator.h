// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "CellularAutomata/Public/Core/PlayerController/GamePlayerController.h"
#include "CellularAutomata/Public/Ui/UiController.h"
#include "Automata/Grid/CellGrid.h"
#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"
#include "GameFramework/PlayerController.h"
#include "AutomataOrchestrator.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;

/** Стратегия хранения клеток сетки автомата. */
UENUM(BlueprintType)
enum class EGridStorageStrategy : uint8
{
	Sparse,
	Dense
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
	float Speed = 1.0f;

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

	/** Инстансированный меш для отрисовки клеток автомата */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Cells")
	UInstancedStaticMeshComponent* CellsMesh;

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
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
	TSubclassOf<UUserWidget> HUDWidgetClass;
	
	
private:
	TUniquePtr<FUiController> UiController;
	TUniquePtr<FCellGrid> Grid;
	TUniquePtr<FInstancedMeshCellGridRenderer> Renderer;

	void InitializeHUD();
	void InitializePlayerController();
	void InitializeRenderer();

	AGamePlayerController* GamePC;
};