#include "Ui/HudElementWidget.h"
#include "Kismet/GameplayStatics.h"
#include "Orchestration/AutomataOrchestrator.h"
#include "Ui/HudFormat.h"

namespace
{
	/** Сводки оркестратора одним видом. Геттеры сводок пересчитывают всё не
	 *  чаще раза за кадр (UpdateHudStats(), guard по GFrameCounter), так что
	 *  десяток элементов, спрашивающих каждый кадр, не множит работу. */
	FHudStatsView MakeView(AAutomataOrchestrator& Orchestrator)
	{
		return FHudStatsView{
			Orchestrator.GetSimulationStats(),
			Orchestrator.GetHudRenderStats(),
			Orchestrator.GetCutStats(),
			Orchestrator.GetPerformanceStats(),
			Orchestrator.GetCameraStats(),
			Orchestrator.GetGeneratorStats(),
			Orchestrator.GetSonificationStats(),
			Orchestrator.GetLastRenderStats()
		};
	}
}

AAutomataOrchestrator* UHudElementWidget::GetOrchestrator()
{
	if (!IsValid(CachedOrchestrator))
	{
		CachedOrchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	}
	return CachedOrchestrator;
}

FText UHudElementWidget::GetLabel()
{
	if (!LabelOverride.IsEmpty())
	{
		return LabelOverride;
	}
	return bIsChip ? HudFormat::ChipLabel(Chip) : HudFormat::MetricLabel(Metric);
}

FText UHudElementWidget::GetMetricValue()
{
	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	if (!Orchestrator)
	{
		// В Designer и в превью оркестратора нет - прочерк вместо пустоты,
		// чтобы вёрстку было видно.
		return INVTEXT("—");
	}
	return HudFormat::MetricValue(Metric, MakeView(*Orchestrator));
}

float UHudElementWidget::GetMetricFraction()
{
	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	float Fraction = 0.0f;
	if (Orchestrator)
	{
		HudFormat::MetricFraction(Metric, MakeView(*Orchestrator), Fraction);
	}
	return Fraction;
}

ESlateVisibility UHudElementWidget::GetFractionVisibility()
{
	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	float Unused = 0.0f;
	const bool bHasFraction = Orchestrator && HudFormat::MetricFraction(Metric, MakeView(*Orchestrator), Unused);
	return bHasFraction ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
}

EHudChipState UHudElementWidget::GetChipState()
{
	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	return Orchestrator ? HudFormat::ChipState(Chip, MakeView(*Orchestrator)) : EHudChipState::Off;
}

FSlateColor UHudElementWidget::GetChipForeground()
{
	return FSlateColor(ColorFor(GetChipState()));
}

FLinearColor UHudElementWidget::GetChipFill()
{
	const EHudChipState State = GetChipState();
	if (State != EHudChipState::Active && State != EHudChipState::Warning)
	{
		return FLinearColor::Transparent;
	}
	FLinearColor Fill = ColorFor(State);
	Fill.A *= FillOpacity;
	return Fill;
}

ESlateVisibility UHudElementWidget::GetBusyVisibility()
{
	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	if (!Orchestrator)
	{
		return ESlateVisibility::Hidden;
	}
	const bool bBusy = Orchestrator->GetSimulationStats().bIsComputing || Orchestrator->GetHudRenderStats().bIsRendering;
	// Hidden, а не Collapsed: индикатор не должен двигать соседей, мигая.
	return bBusy ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden;
}

FLinearColor UHudElementWidget::ColorFor(EHudChipState State) const
{
	switch (State)
	{
	case EHudChipState::Active:  return ActiveColor;
	case EHudChipState::Armed:   return ArmedColor;
	case EHudChipState::Warning: return WarningColor;
	default:                     return OffColor;
	}
}
