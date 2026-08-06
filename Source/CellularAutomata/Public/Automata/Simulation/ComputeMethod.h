#pragma once

#include "CoreMinimal.h"
#include "ComputeMethod.generated.h"

/** Метод расчёта шага симуляции. Обе реализации настоящие: Cpu - параллельный
 *  алгоритм с bucket-partitioned дедупом кандидатов, Gpu - RDG compute-шейдер
 *  (см. FGpuComputeStrategy; на CPU он откатывается только точечно, когда
 *  объём AABB не влезает в GpuVolumeCellLimit, с предупреждением в лог).
 *  Только Gpu умеет считать пачку поколений за один круг - см.
 *  FCellularAutomatonComputeStrategy::StepBatch(). */
UENUM(BlueprintType)
enum class EComputeMethod : uint8
{
	Cpu,
	Gpu
};
