#pragma once

#include "CoreMinimal.h"
#include "Types/SlateEnums.h"
#include "Ui/HudElementWidget.h"
#include "HudRuleInput.generated.h"

class UEditableTextBox;
class UTextBlock;

/**
 * Поле ввода правила в HUD (WBP_HudRuleInput): строка вида
 * "Survival/Birth/States/Neighborhood", Enter применяет её через
 * TryApplyRuleString(). Ошибка разбора показывается под полем, а не только в
 * лог - правило набирают руками, и опечатка должна быть видна там же.
 *
 * Пока поле не в фокусе, в нём строка правила, которое считается сейчас
 * (GetActiveRuleString(), а не UPROPERTY RuleString - см. docs/hud.md), так
 * что смена пресета или хоткея сразу видна и здесь. Пока в фокусе - текст
 * пользователя не трогается.
 *
 * Хоткеи на время набора глушит AGamePlayerController::InputKey(): иначе
 * буква R в "5-7/6/2/E" сбрасывала бы симуляцию, а N перекатывала сид.
 */
UCLASS(Abstract)
class CELLULARAUTOMATA_API UHudRuleInput : public UHudElementWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Имена дочерних виджетов в WBP: "RuleBox" и "ErrorText". */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UEditableTextBox> RuleBox;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> ErrorText;

private:
	UFUNCTION()
	void HandleCommitted(const FText& Text, ETextCommit::Type CommitMethod);

	void SetError(const FString& Message);
};
