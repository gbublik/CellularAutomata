#pragma once

#include "CoreMinimal.h"
#include "HudMetric.generated.h"

/** Какую величину показывает один элемент HUD (UHudElementWidget::Metric).
 *
 *  Перечисление, а не набор отдельных геттеров, ради одного свойства: виджет
 *  показателя в UMG делается ОДИН (WBP_HudStat) и расставляется по экрану
 *  сколько угодно раз, а что именно он показывает, решает это поле в
 *  Details-панели экземпляра. Текст и подпись строит HudFormat::MetricValue()/
 *  MetricLabel() - вне виджета, чтобы их можно было проверить тестом.
 *
 *  Порядок значений ни на что не завязан, кроме сохранённых WBP: экземпляры
 *  хранят значение по ИМЕНИ, так что новое значение можно вставлять куда
 *  угодно, а переименование старого сломает расставленные виджеты. */
UENUM(BlueprintType)
enum class EHudMetric : uint8
{
	Generation,
	GenerationRate,
	TargetSpeed,
	Alive,
	Selected,
	Fps,
	Compute,
	StepsPerRender,
	Upload,
	GpuUpload,
	Vram,
	SystemMemory,
	DrawnCells,
	RenderProfile,
	Rule,
	RunState,
	Generator,
	GeneratorCells,
	Sound,
	CameraSpeed,
	AutoReseed
};
