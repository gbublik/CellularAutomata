#pragma once

#include "CoreMinimal.h"
#include "HudPresetKind.generated.h"

/** Какую таблицу пресетов показывает выпадающий список HUD
 *  (UHudPresetSelector::Kind). Каждая - пара GetXPresets()/ApplyXPreset(Index)
 *  на оркестраторе, то есть общая форма таблиц пресетов проекта; селектор
 *  просто знает, какую пару звать. */
UENUM(BlueprintType)
enum class EHudPresetKind : uint8
{
	Rule,
	CellShape,
	Generator,
	Render,
	Light,
	Sound
};
