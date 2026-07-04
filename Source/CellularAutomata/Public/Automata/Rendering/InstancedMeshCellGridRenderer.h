#pragma once

#include "CoreMinimal.h"
#include "Automata/Rendering/CellGridRenderer.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;

/** Разбивка времени последнего Render() по этапам, в секундах - для
 *  профилирования. Логирование намеренно вынесено из Render() к
 *  вызывающей стороне (см. GetLastRenderTimings()), иначе сама операция
 *  логирования (форматирование строки + запись в файл) съедала бы время
 *  ещё до того, как её саму успели бы измерить. */
struct FRenderTimings
{
	double SetMeshSeconds = 0.0;
	double ClearSeconds = 0.0;
	double ScaleSeconds = 0.0;
	double GetAliveSeconds = 0.0;
	double BuildTransformsSeconds = 0.0;
	double AddInstanceSeconds = 0.0;
};

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

	const FRenderTimings& GetLastRenderTimings() const { return LastTimings; }

	/** Компонент, который этот рендерер оборачивает - нужен, чтобы вызывающая
	 *  сторона могла определить, устарел ли рендерер (обёрнут не тот
	 *  компонент), не храня свою копию указателя параллельно. */
	UInstancedStaticMeshComponent* GetComponent() const { return Component.Get(); }

private:
	TWeakObjectPtr<UInstancedStaticMeshComponent> Component;
	TWeakObjectPtr<UStaticMesh> Mesh;
	TWeakObjectPtr<UMaterialInterface> Material;
	FRenderTimings LastTimings;
};
