#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Ui/HudChip.h"
#include "Ui/HudChipState.h"
#include "Ui/HudMetric.h"
#include "HudElementWidget.generated.h"

class AAutomataOrchestrator;

/**
 * Базовый класс мелких элементов HUD: показателя (WBP_HudStat) и чипа режима
 * (WBP_HudChip). Один WBP на вид элемента, а что он показывает, решает поле
 * экземпляра - Metric или Chip - в Details-панели главного HUD.
 *
 * Все геттеры здесь - pure и без аргументов, ровно того вида, который
 * принимает привязка свойства UMG. Поэтому у WBP-наследников нет графа: текст
 * подписи привязан к GetLabel(), значение к GetMetricValue(), цвета чипа к
 * GetChipForeground()/GetChipFill(). Что и как считать, живёт в HudFormat
 * (тестируемо), как выглядит - в UMG и в цветах ниже (настраиваются в
 * Designer, а не в коде).
 *
 * Оркестратор ищется так же, как в UMainHudWidget - GetActorOfClass() с
 * кэшем и IsValid()-ревалидацией; каждый элемент держит свою ссылку, поиск
 * происходит один раз на элемент.
 */
UCLASS(Abstract)
class CELLULARAUTOMATA_API UHudElementWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Что показывает элемент-показатель. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD")
	EHudMetric Metric = EHudMetric::Generation;

	/** Какой режим отражает элемент-чип. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD")
	EHudChip Chip = EHudChip::Culling;

	/** Своя подпись вместо стандартной (HudFormat::MetricLabel()/ChipLabel()).
	 *  Пусто - стандартная. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD")
	FText LabelOverride;

	/** true - элемент работает как чип (GetLabel() отдаёт подпись чипа), false -
	 *  как показатель. Отдельный флаг, потому что у одного класса оба поля. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD")
	bool bIsChip = false;

	/** Цвета состояний чипа. Fill - заливка под подписью; у Armed и Off её нет,
	 *  только приглушённый текст, чтобы горящие чипы читались издалека. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD|Style")
	FLinearColor ActiveColor = FLinearColor(0.20f, 0.74f, 0.95f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD|Style")
	FLinearColor ArmedColor = FLinearColor(0.20f, 0.74f, 0.95f, 0.55f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD|Style")
	FLinearColor OffColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.28f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD|Style")
	FLinearColor WarningColor = FLinearColor(1.0f, 0.62f, 0.18f, 1.0f);

	/** Непрозрачность заливки горящего чипа относительно его цвета. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD|Style", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FillOpacity = 0.16f;

	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	AAutomataOrchestrator* GetOrchestrator();

	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	FText GetLabel();

	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	FText GetMetricValue();

	/** Доля 0..1 для полоски-прогресса под показателем; 0, если её нет. */
	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	float GetMetricFraction();

	/** Collapsed, если у показателя нет доли - полоска не занимает место. */
	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	ESlateVisibility GetFractionVisibility();

	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	EHudChipState GetChipState();

	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	FSlateColor GetChipForeground();

	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	FLinearColor GetChipFill();

	/** Идёт фоновый шаг или чанковый рендер - для индикатора занятости. */
	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	ESlateVisibility GetBusyVisibility();

private:
	FLinearColor ColorFor(EHudChipState State) const;

	UPROPERTY(Transient)
	TObjectPtr<AAutomataOrchestrator> CachedOrchestrator = nullptr;
};
