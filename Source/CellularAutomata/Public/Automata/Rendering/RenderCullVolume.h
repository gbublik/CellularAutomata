#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RenderCullVolume.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class UMaterialInterface;

/** Какую ручку манипулятора трогает пользователь - см.
 *  ARenderCullVolume::TraceGizmoHandle(). Порядок осей X/Y/Z намеренно
 *  совпадает с порядком компонент FVector, чтобы индекс оси можно было
 *  получать арифметикой, а не switch'ом. */
UENUM(BlueprintType)
enum class EVolumeGizmoHandle : uint8
{
	None,
	TranslateX,
	TranslateY,
	TranslateZ,
	ScaleX,
	ScaleY,
	ScaleZ
};

/**
 * Плейсируемый в уровне куб, отсекающий клетки вне себя ДО построения
 * FTransform/AddInstances (см. AAutomataOrchestrator::BuildAgeBuckets() ->
 * FCellGrid::GetAliveCellsInBounds()) - в отличие от CellCullStartDistance/
 * CellCullEndDistance, которые прячут уже построенные инстансы post-hoc и
 * не снижают стоимость самого построения. AAutomataOrchestrator находит
 * этот актёр в мире сам через UGameplayStatics::GetActorOfClass() (тот же
 * идиом, что AGamePlayerController использует для поиска оркестратора) -
 * никакой ручной ссылки в Details panel не требуется.
 *
 * Двигается/масштабируется обычным гизмо актёра - отдельного UI нет, Size и
 * Location не нужны отдельными свойствами: Location - обычный Transform
 * актёра, Size - BoxExtent компонента BoundsBox (редактируется в Details
 * panel и ручками ресайза на самом кубе во вьюпорте). Сознательно
 * axis-aligned only: поворот актёра не наклоняет куб отсечения,
 * GetWorldBounds() всегда строит AABB из локации и масштабированного
 * BoxExtent - тот же дух упрощения, что и у CellSelection::
 * SelectCellsInScreenRect()'s "без ограничения по глубине".
 */
UCLASS(meta = (DisplayName = "Render Cull Volume"))
class CELLULARAUTOMATA_API ARenderCullVolume : public AActor
{
	GENERATED_BODY()

public:
	ARenderCullVolume();

	/** Axis-aligned мировые границы куба (Location +- масштабированный
	 *  BoxExtent) - см. doc-comment класса за тем, почему без поворота. */
	UFUNCTION(BlueprintCallable, Category = "Automata|Rendering")
	FBox GetWorldBounds() const;

	/** Показать/скрыть манипулятор (стрелки перемещения и кубики масштаба).
	 *  В редакторе он не нужен - там куб двигает и масштабирует штатный гизмо
	 *  актёра, а PostEditMove() уже дёргает перерисовку; смысл этих ручек
	 *  только в PIE, где никакого гизмо нет. Поэтому по умолчанию скрыт и
	 *  показывается контроллером вместе с режимом взаимодействия мышью
	 *  (Tab) - вне его всё равно нет курсора, чтобы за них взяться. */
	void SetGizmoVisible(bool bVisible);

	bool IsGizmoVisible() const { return bGizmoVisible; }

	/** Подгоняет масштаб манипулятора так, чтобы на экране он оставался
	 *  примерно одного размера независимо от расстояния до камеры - как в 3D-
	 *  редакторах. Без этого ручки либо теряются в клетках, либо застилают
	 *  весь экран: куб отсечения в этом проекте масштабируют десятками, а
	 *  клетка - это 100 юнитов. Зовётся каждый кадр, пока манипулятор виден. */
	void UpdateGizmoScreenSize(const FVector& CameraLocation, float CameraFOVDegrees);

	/** Какая ручка (если есть) лежит под лучом из курсора. Своя трассировка,
	 *  а не движковая: у ручек намеренно нет коллизии (как и у всего
	 *  остального в этом проекте), поэтому проверяем луч против капсулы вдоль
	 *  оси - дёшево и не требует ни коллизионных каналов, ни профилей.
	 *  OutAxis - направление оси ручки в МИРЕ, нужно для последующего драга. */
	EVolumeGizmoHandle TraceGizmoHandle(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutAxis) const;

	/** Начать перетаскивание ручки. AxisParam - положение курсора вдоль оси
	 *  ручки в мировых единицах (ближайшая к лучу точка оси, считает
	 *  вызывающий - у него есть камера); дальше все смещения меряются от
	 *  него, поэтому ручка не "прыгает" под курсор в момент захвата. */
	void BeginGizmoDrag(EVolumeGizmoHandle Handle, const FVector& Axis, float AxisParam);

	/** Продолжить перетаскивание: двигает актёр либо меняет BoxExtent по оси
	 *  на разницу с AxisParam из BeginGizmoDrag(). Перерисовку клеток НЕ
	 *  запускает - только на EndGizmoDrag(), ровно как PostEditMove() в
	 *  редакторе ждёт bFinished: на миллионах клеток полный ререндер на
	 *  каждый кадр драга свёл бы всю затею на нет, а проволочный куб и так
	 *  двигается за курсором и даёт обратную связь. */
	void UpdateGizmoDrag(float AxisParam);

	/** Завершить перетаскивание и попросить оркестратор перерисоваться. */
	void EndGizmoDrag();

	bool IsGizmoDragging() const { return ActiveGizmoHandle != EVolumeGizmoHandle::None; }

	/** Ось активной ручки в мире - вызывающему нужна, чтобы каждый кадр
	 *  пересчитывать AxisParam для UpdateGizmoDrag(). */
	FVector GetActiveGizmoAxis() const { return ActiveGizmoAxis; }

#if WITH_EDITOR
	/** Перерисовку по завершении перетаскивания/ресайза куба нужно
	 *  триггерить явно - PostEditMove(bFinished) это штатный колбэк
	 *  движка ровно для этого (тот же, которым пользуется встроенный
	 *  AVolume после ресайза брашем); при bFinished==true находим
	 *  AAutomataOrchestrator в мире (тот же GetActorOfClass()-идиом, что и
	 *  везде в проекте) и просим его перерисовать текущий кадр немедленно.
	 *  PostEditChangeProperty() дополнительно ловит правку точных чисел в
	 *  Details panel (не драг) - там колбэк всегда "уже закончили",
	 *  доп. проверка bFinished не нужна. */
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Материалы ручек по осям X/Y/Z - назначаются в Details panel, как
	 *  SelectionMaterial/GhostShapeMaterial/BakedMeshMaterial. Не обязательны:
	 *  без них манипулятор работает, но рисуется дефолтным материалом меша,
	 *  то есть все три оси одинаковые на вид - о чём говорит лог, чтобы это
	 *  не выглядело поломкой. Ожидаются простые unlit-цвета (красный/зелёный/
	 *  синий по традиции 3D-редакторов). */
	UPROPERTY(EditAnywhere, Category = "Automata|Gizmo")
	TObjectPtr<UMaterialInterface> AxisMaterialX;

	UPROPERTY(EditAnywhere, Category = "Automata|Gizmo")
	TObjectPtr<UMaterialInterface> AxisMaterialY;

	UPROPERTY(EditAnywhere, Category = "Automata|Gizmo")
	TObjectPtr<UMaterialInterface> AxisMaterialZ;

	/** Экранный размер манипулятора - доля полувысоты вьюпорта, которую
	 *  занимает ось от центра до кончика стрелки. 0.15 - примерно то же
	 *  ощущение, что у гизмо в редакторе. */
	UPROPERTY(EditAnywhere, Category = "Automata|Gizmo", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float GizmoScreenSize = 0.15f;

private:
	/** Строит ручки в конструкторе. Меши берутся из /Engine/BasicShapes
	 *  (Cylinder - стержень, Cone - наконечник, Cube - ручка масштаба):
	 *  они есть в любом проекте, в отличие от /Engine/EditorMeshes, который
	 *  editor-only и в игру не попадает. */
	void BuildGizmoComponents();

	/** Длина оси манипулятора в ЛОКАЛЬНЫХ единицах до применения экранного
	 *  масштаба - вся геометрия ниже разложена относительно неё. */
	static constexpr float GizmoAxisLength = 100.0f;

	/** Радиус, в пределах которого луч считается попавшим в ручку (в тех же
	 *  локальных единицах). Заметно больше самой геометрии стержня - целиться
	 *  мышью в двухюнитовый цилиндр невозможно, а в 3D-редакторах зона
	 *  захвата тоже щедрее нарисованного. */
	static constexpr float GizmoPickRadius = 12.0f;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> GizmoRoot;

	/** По три на ось, индекс = ось (0=X, 1=Y, 2=Z) - см. EVolumeGizmoHandle. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> TranslateShafts;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> TranslateHeads;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> ScaleHandles;

	bool bGizmoVisible = false;

	/** Состояние активного драга (см. BeginGizmoDrag()). None - драга нет. */
	EVolumeGizmoHandle ActiveGizmoHandle = EVolumeGizmoHandle::None;
	FVector ActiveGizmoAxis = FVector::ZeroVector;
	float DragStartAxisParam = 0.0f;
	FVector DragStartActorLocation = FVector::ZeroVector;
	FVector DragStartBoxExtent = FVector::ZeroVector;

	/** Резолвит AAutomataOrchestrator и просит перерисовать текущий кадр.
	 *  Раньше существовал только под WITH_EDITOR (для PostEditMove()) - теперь
	 *  нужен и в игре, по завершении драга ручки. */
	void NotifyOrchestratorToRefresh() const;

	/** Текущий экранный масштаб GizmoRoot - им же пересчитываются радиусы
	 *  попадания в TraceGizmoHandle(), иначе целиться пришлось бы в
	 *  геометрию, которой на экране нет. */
	float GizmoWorldScale = 1.0f;

	/** Root component - визуализируется в редакторе и в PIE (HiddenInGame
	 *  false, см. конструктор) как проволочный куб, удобно подгонять
	 *  границы на лету. Коллизия отключена - чисто логический маркер,
	 *  та же конвенция, что у CellsMeshFlat/CellsMeshHierarchical. */
	UPROPERTY(VisibleAnywhere, Category = "Automata|Rendering")
	TObjectPtr<UBoxComponent> BoundsBox;
};
