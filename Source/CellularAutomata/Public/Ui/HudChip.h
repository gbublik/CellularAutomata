#pragma once

#include "CoreMinimal.h"
#include "HudChip.generated.h"

/** Какой режим показывает индикатор-"чип" HUD (UHudElementWidget::Chip).
 *  Та же идея, что EHudMetric: один виджет чипа в UMG (WBP_HudChip), а что он
 *  отражает - поле экземпляра. Состояние вычисляет HudFormat::ChipState(). */
UENUM(BlueprintType)
enum class EHudChip : uint8
{
	Culling,
	Volume,
	Slab,
	AgeFilter,
	Ghost,
	Shadows,
	Background,
	Sound,
	Selection,
	AutoReseed,
	Ortho,
	Headlight,
	Chunked,
	ComputeFallback
};
