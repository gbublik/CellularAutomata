#pragma once

#include "CoreMinimal.h"
#include "Automata/Rendering/CellRenderStats.h"
#include "Ui/HudChip.h"
#include "Ui/HudChipState.h"
#include "Ui/HudMetric.h"
#include "Ui/HudStats.h"

/** Все сводки HUD одним аргументом - ссылками, не копиями: элемент HUD
 *  спрашивает свой текст каждый кадр, и копировать семь структур со строками
 *  ради одного числа незачем. В тесте ссылки указывают на локальные структуры. */
struct FHudStatsView
{
	const FHudSimulationStats& Simulation;
	const FHudRenderStats& Render;
	const FHudCutStats& Cuts;
	const FHudPerformanceStats& Performance;
	const FHudCameraStats& Camera;
	const FHudGeneratorStats& Generator;
	const FHudSonificationStats& Sonification;
	const FCellRenderStats& RenderCounts;
};

/** Текст и состояния элементов HUD - свободные функции, а не методы виджета,
 *  по общему правилу проекта: так их видит автотест без мира и без UMG.
 *
 *  Числа форматируются вручную (FString::Printf), а не через FText::AsNumber:
 *  тот берёт разделители из текущей культуры, и в русской локали "1.5K"
 *  превращается в "1,5K" - тест стал бы зависеть от языка системы, а HUD от
 *  настроек редактора. */
namespace HudFormat
{
	/** Целое с разделителем тысяч - пробелом: "12 345", "-7 000". */
	CELLULARAUTOMATA_API FString GroupedInteger(int64 Value);

	/** Компактная запись счётчика, три значащие цифры: до 10 000 - целиком
	 *  ("9 876"), дальше "12.3K", "456K", "1.24M", "12.3M", "1.20B". Единица
	 *  выбирается ПОСЛЕ округления, поэтому 999 950 даёт "1.00M", а не "1000K". */
	CELLULARAUTOMATA_API FString CompactCount(int64 Value);

	/** Скорость поколений: одна цифра после точки до 100, дальше целым. */
	CELLULARAUTOMATA_API FString Rate(double Value);

	CELLULARAUTOMATA_API FText MetricLabel(EHudMetric Metric);
	CELLULARAUTOMATA_API FText MetricValue(EHudMetric Metric, const FHudStatsView& Stats);

	/** Доля 0..1 для полоски под показателем - у тех, у кого она есть
	 *  (память, нарисовано/всего, факт/цель скорости). false - полоски нет. */
	CELLULARAUTOMATA_API bool MetricFraction(EHudMetric Metric, const FHudStatsView& Stats, float& OutFraction);

	CELLULARAUTOMATA_API FText ChipLabel(EHudChip Chip);
	CELLULARAUTOMATA_API EHudChipState ChipState(EHudChip Chip, const FHudStatsView& Stats);
}
