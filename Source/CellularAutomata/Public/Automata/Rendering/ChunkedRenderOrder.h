#pragma once

#include "CoreMinimal.h"
#include "ChunkedRenderOrder.generated.h"

/** Порядок, в котором BeginRender() раскладывает живые клетки в
 *  PendingTransforms перед "разлитым" по кадрам реавилом
 *  (AdvanceRenderChunk()) - влияет только на порядок появления инстансов
 *  по кадрам, не на итоговый набор клеток и не на однократный Render(). */
UENUM(BlueprintType)
enum class EChunkedRenderOrder : uint8
{
	/** Порядок из FCellGrid::GetAliveCells() как есть - для FDenseCellGrid
	 *  это порядок обхода TMap<FIntVector, FChunk> (по чанкам), поэтому
	 *  реавил идёт заметными "блобами" один за другим. */
	Sequential,

	/** То же самое, но развёрнутое задом наперёд. */
	SequentialReversed,

	/** Один Algo::RandomShuffle перед нарезкой на чанки - каждый кадровый
	 *  срез становится случайной равномерной выборкой по всему объёму,
	 *  силуэт сетки виден почти сразу, дальше просто "доуплотняется". */
	Shuffled,

	/** Сортировка по расстоянию до камеры (GamePC->PlayerCameraManager) -
	 *  ближние клетки первыми. */
	DistanceFromCameraNearFirst,

	/** Сортировка по расстоянию до камеры - дальние клетки первыми. */
	DistanceFromCameraFarFirst,

	/** Сортировка по расстоянию до центра ограничивающего объёма всех живых
	 *  клеток текущего кадра - реавил "расползается" наружу от центра
	 *  сетки. Не привязан к внутренней чанковой сетке FDenseCellGrid
	 *  (координаты центра считаются заново по AliveCells каждый вызов) -
	 *  рендерер остаётся grid-агностичным. */
	FromCenterOutward,
};
