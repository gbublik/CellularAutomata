#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "CellGpuInstancesComponent.generated.h"

/**
 * ПРОТОТИП (docs/rendering.md, "Drawing without the CPU"): экземпляры, у
 * которых нет CPU-копии - их данные существуют только в GPU-Scene и пишутся
 * compute-шейдером, а рисует их обычный Nanite::FSceneProxy. Та же схема, что
 * у PCG (UPCGProceduralISMComponent - он приватный, поэтому повторён здесь по
 * образцу, на экспортированных кирпичах движка).
 *
 * Сейчас пишет только тестовый узор (куб GridDim^3 с шагом Spacing) - цель
 * прототипа ответить на один вопрос: даёт ли этот путь FPS уровня HISM с
 * Nanite (~96 на 4.7M), а не уровня Niagara (~18 на 5.1M).
 *
 * Ограничения пути, которые здесь соблюдаются:
 *  - ёмкость (Capacity) задаётся на CPU; смена ёмкости пересоздаёт прокси и
 *    ТЕРЯЕТ записанные данные - запись повторяется, как только появится новый
 *    прокси (см. TickComponent);
 *  - Lumen surface cache и distance fields для GPU-only экземпляров не
 *    поддерживаются (в движке там check) - отключаются в ValidateSetup().
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class CELLULARAUTOMATA_API UCellGpuInstancesComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UCellGpuInstancesComponent();

	/** Показать тестовый узор: куб из GridDim^3 экземпляров с шагом Spacing,
	 *  центрированный на компоненте; MeshScale - масштаб меша экземпляра. */
	UFUNCTION(BlueprintCallable, Category = "Automata|GpuInstances")
	void ShowTestPattern(int32 InGridDim, float InSpacing, float InMeshScale);

	UFUNCTION(BlueprintPure, Category = "Automata|GpuInstances")
	int32 GetCapacity() const { return Capacity; }

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual bool ShouldCreatePhysicsState() const override { return false; }
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual FPrimitiveSceneProxy* CreateStaticMeshSceneProxy(Nanite::FMaterialAudit& NaniteMaterials, bool bCreateNanite) override;

private:
	void ValidateSetup();
	void EnqueueWrite();

	UPROPERTY(EditAnywhere, Category = "Automata|GpuInstances")
	int32 GridDim = 0;

	UPROPERTY(EditAnywhere, Category = "Automata|GpuInstances")
	float Spacing = 150.0f;

	UPROPERTY(EditAnywhere, Category = "Automata|GpuInstances")
	float MeshScale = 0.5f;

	/** Сколько слотов GPU-Scene занимает примитив (GridDim^3). */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Automata|GpuInstances")
	int32 Capacity = 0;

	/** Прокси, для которого данные уже записаны. Только для сравнения
	 *  указателей - никогда не разыменовывается (это не UObject). */
	const FPrimitiveSceneProxy* WrittenForProxy = nullptr;

	/** Кадров с момента появления текущего прокси: запись откладывается на
	 *  пару кадров, пока GPU-Scene не выделит примитиву слоты (то же делает
	 *  PCG, "Primitive instances not allocated in GPU scene, retrying"). */
	int32 FramesSinceProxy = 0;
	const FPrimitiveSceneProxy* SeenProxy = nullptr;
};

/** Актор-носитель для прототипа: его спавнят из Python прямо в PIE. */
UCLASS()
class CELLULARAUTOMATA_API ACellGpuInstancesTestActor : public AActor
{
	GENERATED_BODY()

public:
	ACellGpuInstancesTestActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Automata|GpuInstances")
	TObjectPtr<UCellGpuInstancesComponent> Instances;
};
