#include "Ui/HudElementWidget.h"
#include "Core/PlayerController/GamePlayerController.h"
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
		// Под мышью выключенный чип чуть проявляется - видно, что он кнопка.
		if (bHovered)
		{
			FLinearColor Hint = ColorFor(EHudChipState::Active);
			Hint.A *= FillOpacity * 0.35f;
			return Hint;
		}
		return FLinearColor::Transparent;
	}
	FLinearColor Fill = ColorFor(State);
	Fill.A *= FillOpacity * (bHovered ? 1.6f : 1.0f);
	return Fill;
}

bool UHudElementWidget::IsClickableChip() const
{
	return bIsChip && AGamePlayerController::IsHudChipClickable(Chip);
}

void UHudElementWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (IsClickableChip())
	{
		SetVisibility(ESlateVisibility::Visible);
		SetCursor(EMouseCursor::Hand);
	}
}

FReply UHudElementWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (IsClickableChip() && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		if (AGamePlayerController* PC = Cast<AGamePlayerController>(GetOwningPlayer()))
		{
			PC->ToggleHudChip(Chip);
		}
		// Поглотить клик: иначе он ушёл бы в игру - выделение, рисование клеток.
		return FReply::Handled();
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

void UHudElementWidget::NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseEnter(InGeometry, InMouseEvent);
	bHovered = IsClickableChip();
}

void UHudElementWidget::NativeOnMouseLeave(const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseLeave(InMouseEvent);
	bHovered = false;
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
