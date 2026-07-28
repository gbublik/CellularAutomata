#pragma once

#include "CoreMinimal.h"
#include "Automata/Rendering/CellGridRenderer.h"
#include "Automata/Rendering/ChunkedRenderOrder.h"

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
	double ReorderSeconds = 0.0;
	/** Построение FTransform'ов. Раньше это был один всплеск в BeginRender()
	 *  на всю сетку разом; теперь копится по чанкам вместе с
	 *  AddInstanceSeconds - трансформы строятся в AdvanceRenderChunk(). */
	double BuildTransformsSeconds = 0.0;
	double AddInstanceSeconds = 0.0;
	/** Запись per-instance цветов (SetCustomData) - копится по чанкам.
	 *  Отдельным полем, а не внутри AddInstanceSeconds: цена там своя
	 *  (цикл CustomDataChanged() по инстансам внутри движка), и мерить её
	 *  надо до того, как оптимизировать - см. историю с CandidateBuild. */
	double CustomDataSeconds = 0.0;
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

	/** Дополнительный множитель поверх масштаба "подогнать меш под CellSize"
	 *  (см. BeginRender()). 1.0 (по умолчанию) - инстанс ровно в размер
	 *  клетки, как всегда и было. Нужен рендереру подсветки выделения:
	 *  кубик того же размера в том же месте, что и обычный кубик клетки,
	 *  даёт z-fighting (мерцание двух совпадающих поверхностей) - множитель
	 *  чуть больше 1 обволакивает обычный кубик и виден с любого угла. */
	void SetScaleMultiplier(float InScaleMultiplier);

	virtual void Render(const FCellGrid& Grid, TArray<FCellRenderInstance>&& Instances) override;

	/** Принимает список инстансов и один раз чистит компонент, но не
	 *  добавляет ни одного инстанса - используется вместе с
	 *  AdvanceRenderChunk(), чтобы "разлить" AddInstances (самую дорогую
	 *  часть Render() при больших сетках) по нескольким кадрам вместо
	 *  одного. Render() сам реализован через BeginRender() + один вызов
	 *  AdvanceRenderChunk() без ограничения - так оба пути не дублируют
	 *  логику (Order/CameraLocation значения не важны в этом случае - весь
	 *  массив всё равно уходит одним кадром).
	 *  Order выбирает, в каком порядке инстансы раскладываются до нарезки на
	 *  чанки (см. EChunkedRenderOrder); CameraLocation используется только
	 *  для DistanceFromCamera*-режимов. Grid нужен только ради GetCellSize()
	 *  и НЕ запоминается: позиции в Instances уже мировые, поэтому
	 *  AdvanceRenderChunk() переживает подмену Grid следующим поколением. */
	void BeginRender(const FCellGrid& Grid, TArray<FCellRenderInstance>&& Instances, EChunkedRenderOrder Order, const FVector& CameraLocation);

	/** Добавляет очередную порцию (до MaxCellsThisChunk) инстансов,
	 *  переданных предыдущему BeginRender(): строит их трансформы, добавляет
	 *  через AddInstances() и записывает их цвета через SetCustomData().
	 *  Возвращает true, если после этого вызова ещё остались недобавленные
	 *  инстансы. BuildTransforms/AddInstance/CustomData-Seconds в LastTimings
	 *  накапливаются по всем чанкам одного цикла BeginRender(), а не
	 *  перезаписываются, чтобы суммарное время было видно целиком. */
	bool AdvanceRenderChunk(int32 MaxCellsThisChunk);

	const FRenderTimings& GetLastRenderTimings() const { return LastTimings; }

	/** Компонент, который этот рендерер оборачивает - нужен, чтобы вызывающая
	 *  сторона могла определить, устарел ли рендерер (обёрнут не тот
	 *  компонент), не храня свою копию указателя параллельно. */
	UInstancedStaticMeshComponent* GetComponent() const { return Component.Get(); }

private:
	TWeakObjectPtr<UInstancedStaticMeshComponent> Component;
	TWeakObjectPtr<UStaticMesh> Mesh;
	TWeakObjectPtr<UMaterialInterface> Material;
	/** См. SetScaleMultiplier(). */
	float ScaleMultiplier = 1.0f;
	FRenderTimings LastTimings;

	/** Инстансы, принятые последним BeginRender() - AdvanceRenderChunk()
	 *  добавляет их порциями начиная с PendingCursor. Трансформы из них
	 *  строятся почанково, а не заранее: FTransform под LWC весит 80 байт
	 *  против 16 у FCellRenderInstance. */
	TArray<FCellRenderInstance> PendingInstances;
	int32 PendingCursor = 0;

	/** Масштаб инстанса ("подогнать меш под CellSize" * ScaleMultiplier),
	 *  посчитанный один раз в BeginRender() - иначе его пришлось бы считать
	 *  в каждом чанке заново или держать ссылку на Grid между кадрами. */
	FVector PendingInstanceScale = FVector::OneVector;

	/** Буферы одного чанка, переиспользуемые между кадрами разлива -
	 *  чтобы не реаллоцировать их каждый кадр. */
	TArray<FTransform> ChunkTransformScratch;
	TArray<float> ChunkCustomDataScratch;
};
