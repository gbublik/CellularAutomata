#pragma once

#include "CoreMinimal.h"
#include "CellRenderStats.generated.h"

/** Метрики последнего BuildCellRenderData() (см. doc-comment внутри неё).
 *  Два разных вида числа с разным смыслом: RenderedCellCount/TotalCellCount -
 *  ПАРА (клеток отрисовано/живых всего в сетке, после отсечения
 *  ARenderCullVolume вс. без него) - показывает масштаб расчётов, сколько
 *  из всей симуляции реально видно на экране. ВНИМАНИЕ: при правилах
 *  Generations (States > 2) RenderedCellCount может ЗАКОННО превышать
 *  TotalCellCount - угасающие клетки рисуются, но живыми не считаются, а
 *  Grid->Num() считает только живых. EstimatedUploadMB - ОДНО
 *  общее число, не пара - это оценка размера данных, которые реально
 *  уходят в AddInstances() (т.е. посчитана от RenderedCellCount, не от
 *  TotalCellCount) - как размер файла: единая величина, а не "до/после".
 *  Считается один раз и хранится здесь, а не пересчитывается заново на
 *  каждого потребителя - читают её и UE_LOG в BuildCellRenderData(), и HUD
 *  (UMainHudWidget) через GetLastRenderStats(), без дублирования подсчёта
 *  и риска разъехаться в цифрах между логом и экраном - тот же идиом, что
 *  FRenderTimings у FInstancedMeshCellGridRenderer. USTRUCT(BlueprintType) -
 *  т.к. читает Blueprint-виджет, не только нативный код (см. сводки
 *  в Ui/HudStats.h за тем же решением). */
USTRUCT(BlueprintType)
struct FCellRenderStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	int32 RenderedCellCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	int32 TotalCellCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	double EstimatedUploadMB = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	int32 BytesPerInstance = 0;
};
