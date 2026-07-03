#pragma once

#include "CoreMinimal.h"
#include "Neighborhood.generated.h"

/** Тип соседства для подсчёта живых соседей клетки в 3D. */
UENUM(BlueprintType)
enum class ENeighborhood : uint8
{
	/** 6 соседей: грани куба (±X, ±Y, ±Z) */
	VonNeumann,
	/** 26 соседей: полный куб 3x3x3 без центра */
	Moore
};
