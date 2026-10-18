#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "RenderGraphResources.h"
#include "CellGpuInstancesComponent.generated.h"

struct FGpuResidentPlanes;
class FRDGBuilder;
class FRDGBuffer;
class FRDGBufferUAV;

/**
 * Что и как рисовать из GPU-сессии: снимок на игровом потоке тех же
 * настроек, по которым BuildCellRenderData() строит HISM-картинку, -
 * решётка, масштаб меша, таблицы цвета, "только поверхность" и три отсечения.
 * Неизменяем после постройки: его читают render thread и шейдеры.
 */
struct CELLULARAUTOMATA_API FCellGpuDrawParams
{
	FVector3f Step = FVector3f(100.0f);
	FVector3f Origin = FVector3f::ZeroVector;
	FVector3f Scale = FVector3f::OneVector;

	/** Соседи "по грани"; пусто - "только поверхность" выключена. */
	TArray<FIntVector4> FaceOffsets;

	/** Рисовать только то, что видно снаружи: клетку, хоть одна грань
	 *  которой смотрит во внешний воздух (CellExteriorAir.usf). Режет поры
	 *  пористой структуры, которые "только поверхность" считает открытыми.
	 *  Ставится только для кубической решётки и без отсечений (у других
	 *  решёток 6-связный воздух течёт сквозь всё, а срез и так вскрывает нутро). */
	bool bExteriorOnly = false;

	/** FColor, упакованный как R | G << 8 | B << 16; по 256 записей. */
	TArray<uint32> AgeLut;
	TArray<uint32> DecayLut;

	bool bAgeFilterActive = false;
	uint32 AgeMask[8] = {};

	bool bCullActive = false;
	FVector3f CullMin = FVector3f::ZeroVector;
	FVector3f CullMax = FVector3f::ZeroVector;

	bool bSliceActive = false;
	FVector3f SliceOrigin = FVector3f::ZeroVector;
	FVector3f SliceForward = FVector3f(1.0f, 0.0f, 0.0f);
	float SliceMinDepth = 0.0f;
	float SliceMaxDepth = 0.0f;
};

/**
 * Состояние рисования, которое живёт между кадрами на render thread:
 * плоскость внешнего воздуха. Её строит заход сессии (в счёте видимых), а
 * читает запись экземпляров в графе сцены - поэтому она не может быть
 * временным буфером одного графа. Один объект на сессию.
 */
struct CELLULARAUTOMATA_API FCellGpuDrawState
{
	FCellGpuDrawState() = default;
	/** Пулированный буфер отпускается на render thread, где бы ни умерла
	 *  последняя ссылка (так же поступает FGpuResidentSimulation). */
	~FCellGpuDrawState();

	TRefCountPtr<FRDGPooledBuffer> Air;
	/** Под какой объём построен воздух; не совпал с плоскостями - не годен. */
	FIntVector AirDim = FIntVector::ZeroValue;
	int32 AirRowWords = 0;
};

namespace CellGpuDrawing
{
	/** Слова сводки сессии после счёта видимых (CounterIndex + ...): раунды
	 *  заливки внешнего воздуха и флаг "не сошлась за потолок раундов". */
	constexpr uint32 AirRoundsWordOffset = 1;
	constexpr uint32 AirUnconvergedWordOffset = 2;
	/** ...и сколько клеток нарисовал бы HISM на паузе (его правило - без
	 *  внешнего воздуха): по этому числу решается, влезет ли пауза в HISM. */
	constexpr uint32 HismVisibleWordOffset = 3;

	/** Построить плоскость внешнего воздуха текущего поколения в
	 *  DrawState.Air (render thread). nullptr - объём пуст или слишком велик. */
	CELLULARAUTOMATA_API FRDGBuffer* AddExteriorAirPasses(FRDGBuilder& GraphBuilder, const FGpuResidentPlanes& Planes,
		FRDGBuffer* Bits, FRDGBuffer* Decay, FCellGpuDrawState& DrawState,
		FRDGBufferUAV* Stats, uint32 RoundsWord, uint32 UnconvergedWord);

	/** Счёт клеток, которые будут нарисованы (CellVisibility.ush), с
	 *  прибавлением в слово CounterIndex буфера Counter. Для
	 *  FGpuResidentSimulation::FExtraPass: ёмкость компонента надо знать на CPU
	 *  до записи, а сводка захода и так читается назад. Render thread. */
	CELLULARAUTOMATA_API void AddCountVisiblePass(FRDGBuilder& GraphBuilder, const FCellGpuDrawParams& Params,
		const FGpuResidentPlanes& Planes, FRDGBuffer* Bits, FRDGBuffer* Ages, FRDGBuffer* Decay,
		FRDGBufferUAV* Counter, uint32 CounterIndex, FCellGpuDrawState* DrawState = nullptr);

	/** Потолок экземпляров на сцену - 24-битный InstanceId (docs/rendering.md). */
	constexpr int32 MaxInstances = 15 * 1024 * 1024;
}

/**
 * Экземпляры, у которых нет CPU-копии: их данные существуют только в
 * GPU-Scene и пишутся compute-шейдером, а рисует их обычный Nanite::FSceneProxy.
 * Та же схема, что у PCG (UPCGProceduralISMComponent - он приватный, поэтому
 * повторён здесь по образцу, на экспортированных кирпичах движка).
 *
 * Два режима:
 *  - ShowResidentCells(): клетки GPU-сессии (FGpuResidentSimulation) прямо из
 *    её буферов - отбор видимых и запись экземпляров одним проходом на каждое
 *    новое поколение, без единого байта через CPU (docs/rendering.md, шаг 2);
 *  - ShowTestPattern(): куб GridDim^3 - прототип, которым мерился FPS пути.
 *
 * Ограничения пути:
 *  - ёмкость (Capacity) задаётся на CPU; смена ёмкости или границ пересоздаёт
 *    прокси и ТЕРЯЕТ записанные данные - запись повторяется, как только у
 *    нового прокси появятся слоты в GPU-Scene (TickComponent). Пересоздание
 *    стоит секунды на миллионах слотов, поэтому ёмкость растёт вдвое;
 *  - Lumen surface cache, distance fields и ray tracing для GPU-only
 *    экземпляров не поддерживаются (в движке там check) - отключаются в
 *    ValidateSetup(). Полноценный свет - у HISM на паузе.
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

	/**
	 * Нарисовать текущее поколение сессии. Зовётся на каждое новое поколение.
	 * VisibleCount - сколько клеток пройдёт отбор (счёт из сводки захода),
	 * по нему выбирается ёмкость. LocalBounds - объём сессии в локальных
	 * координатах компонента. ReplacedComponent прячется, как только первая
	 * запись ушла в GPU-Scene (до этого он показывает прежнюю картинку), и
	 * возвращается в ClearCells().
	 */
	void ShowResidentCells(const TSharedRef<FGpuResidentPlanes, ESPMode::ThreadSafe>& Planes,
		const TSharedRef<const FCellGpuDrawParams, ESPMode::ThreadSafe>& Params,
		const TSharedRef<FCellGpuDrawState, ESPMode::ThreadSafe>& DrawState,
		int64 VisibleCount, const FBox& LocalBounds, UPrimitiveComponent* ReplacedComponent);

	/** Убрать клетки (прокси уничтожается, слоты GPU-Scene освобождаются) и
	 *  вернуть видимость компоненту, который они заменяли. */
	void ClearCells();

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
	void EnqueueTestPatternWrite();
	void EnqueueCellsWrite();
	void SetCapacityAndBounds(int32 NewCapacity, const FBox& NewBounds);

	UPROPERTY(EditAnywhere, Category = "Automata|GpuInstances")
	int32 GridDim = 0;

	UPROPERTY(EditAnywhere, Category = "Automata|GpuInstances")
	float Spacing = 150.0f;

	UPROPERTY(EditAnywhere, Category = "Automata|GpuInstances")
	float MeshScale = 0.5f;

	/** Сколько слотов GPU-Scene занимает примитив. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Automata|GpuInstances")
	int32 Capacity = 0;

	/** Компонент, который клетки сессии заменяют на экране (HISM клеток). */
	UPROPERTY(Transient)
	TWeakObjectPtr<UPrimitiveComponent> ReplacedComponent;

	/** Режим клеток сессии (иначе - тестовый узор). */
	bool bCellsMode = false;
	/** Новое поколение ждёт записи (или прокси пересоздан и данные потеряны). */
	bool bCellsWritePending = false;
	TSharedPtr<FGpuResidentPlanes, ESPMode::ThreadSafe> CellPlanes;
	TSharedPtr<const FCellGpuDrawParams, ESPMode::ThreadSafe> CellParams;
	TSharedPtr<FCellGpuDrawState, ESPMode::ThreadSafe> CellDrawState;
	FBox CellBounds = FBox(ForceInit);
	bool bWarnedOverCeiling = false;

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
