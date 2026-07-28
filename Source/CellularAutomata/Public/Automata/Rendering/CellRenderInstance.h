#pragma once

#include "CoreMinimal.h"

/** Сколько float'ов per-instance custom data рендерер пишет на каждый инстанс:
 *  RGB цвета клетки. Материал обязан читать их узлом
 *  PerInstanceCustomData3Vector с DataIndex=0 - без такого узла движок вообще
 *  не создаёт буфер custom data (флаг выводится из материала, см.
 *  MakeInstanceDataFlags() в InstancedStaticMesh.cpp) и все клетки выйдут
 *  одного цвета, БЕЗ единой ошибки в логе. */
inline constexpr int32 CellCustomDataFloats = 3;

/** Единица работы рендера клеток: где нарисовать кубик и каким цветом.
 *
 *  Position - УЖЕ мировая координата (результат FCellGrid::GridToWorld()), а
 *  не координата клетки, и это принципиально: AdvanceRenderChunk() растянут по
 *  кадрам, а ApplyStepResult() за это время подменяет TUniquePtr<FCellGrid>
 *  Grid следующим поколением - держать ссылку на сетку между BeginRender() и
 *  последним чанком нельзя. Побочно это позволило вообще убрать прежний
 *  TArray<FTransform> PendingTransforms: sizeof(FTransform) под LWC - 80 байт
 *  (FQuat4d 32 + два FVector3d по 24), т.е. 560 МБ при 7 млн клеток против
 *  112 МБ здесь, а сами трансформы строятся теперь по чанку, размазанно по
 *  кадрам, вместо одного всплеска в BeginRender().
 *
 *  Цвет - FColor, а не FLinearColor: 16 байт против 28 (+75% памяти при тех же
 *  7 млн клеток), а квантование 8 бит на канал на плоско окрашенных кубиках
 *  не видно. ВАЖНО: конвертировать строго через ToFColor() с bSRGB=false -
 *  PerInstanceCustomData это сырой float, никакого sRGB-декода материал не
 *  делает, поэтому квантование обязано быть линейным. bSRGB=true тихо загамит
 *  всю рампу, без следа в логе. Яркость выше 1.0 (свечение) задаётся не здесь,
 *  а скалярным параметром на Material Instance. */
struct FCellRenderInstance
{
	FVector3f Position;
	FColor Color;
};

static_assert(sizeof(FCellRenderInstance) == 16,
	"FCellRenderInstance должен быть ровно 16 байт - при миллионах клеток каждый лишний байт это мегабайты");
