#pragma once

#include "CoreMinimal.h"
#include "HudChipState.generated.h"

/** Состояние индикатора HUD. Три уровня вместо "вкл/выкл" - потому что у
 *  отсечений включённость и действие расходятся (см. docs/hud.md про
 *  bCellCullingActive): Armed - включено, но сейчас ничего не режет; Active -
 *  режет. Warning - отдельный цвет для того, что требует внимания (откат GPU
 *  на CPU). */
UENUM(BlueprintType)
enum class EHudChipState : uint8
{
	Off,
	Armed,
	Active,
	Warning
};
