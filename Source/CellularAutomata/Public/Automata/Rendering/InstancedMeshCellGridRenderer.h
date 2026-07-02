#pragma once

#include "CoreMinimal.h"
#include "Automata/Rendering/CellGridRenderer.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;

/** Рендерит через UInstancedStaticMeshComponent: один инстанс меша на
 *  живую клетку. Компонент создаётся/владеется актором
 *  (CreateDefaultSubobject); этот класс хранит только слабую ссылку. */
class CELLULARAUTOMATA_API FInstancedMeshCellGridRenderer : public FCellGridRenderer
{
public:
	explicit FInstancedMeshCellGridRenderer(UInstancedStaticMeshComponent* InComponent);

	/** Отдельно от конструктора - это дизайнерские свойства, которые
	 *  могут меняться между вызовами Render(). */
	void SetMesh(UStaticMesh* InMesh);
	void SetMaterial(UMaterialInterface* InMaterial);

	virtual void Render(const FCellGrid& Grid) override;

private:
	TWeakObjectPtr<UInstancedStaticMeshComponent> Component;
	TWeakObjectPtr<UStaticMesh> Mesh;
	TWeakObjectPtr<UMaterialInterface> Material;
};
