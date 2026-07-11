#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RenderCullVolume.generated.h"

class UBoxComponent;

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

private:
#if WITH_EDITOR
	/** Общая часть PostEditMove()/PostEditChangeProperty() - резолвит
	 *  AAutomataOrchestrator и просит его перерисовать текущий кадр. */
	void NotifyOrchestratorToRefresh() const;
#endif

	/** Root component - визуализируется в редакторе и в PIE (HiddenInGame
	 *  false, см. конструктор) как проволочный куб, удобно подгонять
	 *  границы на лету. Коллизия отключена - чисто логический маркер,
	 *  та же конвенция, что у CellsMeshFlat/CellsMeshHierarchical. */
	UPROPERTY(VisibleAnywhere, Category = "Automata|Rendering")
	TObjectPtr<UBoxComponent> BoundsBox;
};
