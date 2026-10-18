#include "Ui/HudRuleInput.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "Framework/Application/SlateApplication.h"
#include "Orchestration/AutomataOrchestrator.h"

void UHudRuleInput::NativeConstruct()
{
	Super::NativeConstruct();

	if (RuleBox)
	{
		RuleBox->OnTextCommitted.AddUniqueDynamic(this, &UHudRuleInput::HandleCommitted);
	}
	SetError(FString());
}

void UHudRuleInput::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	if (!RuleBox || !Orchestrator || RuleBox->HasKeyboardFocus())
	{
		return;
	}

	const FString Active = Orchestrator->GetActiveRuleString();
	if (!RuleBox->GetText().ToString().Equals(Active))
	{
		RuleBox->SetText(FText::FromString(Active));
	}
}

void UHudRuleInput::HandleCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	// Только Enter. Уход фокуса (клик мимо) правило не применяет - иначе
	// недописанная строка применялась бы при любом случайном клике по сцене.
	if (CommitMethod != ETextCommit::OnEnter)
	{
		SetError(FString());
		return;
	}

	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	if (!Orchestrator)
	{
		return;
	}

	FString Error;
	if (Orchestrator->TryApplyRuleString(Text.ToString().TrimStartAndEnd(), Error))
	{
		SetError(FString());
		// Вернуть клавиатуру игре: после Enter пользователь ждёт, что пробел
		// снова запускает симуляцию, а не печатает пробел в поле.
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetAllUserFocusToGameViewport();
		}
	}
	else
	{
		SetError(Error);
	}
}

void UHudRuleInput::SetError(const FString& Message)
{
	if (!ErrorText)
	{
		return;
	}
	ErrorText->SetText(FText::FromString(Message));
	ErrorText->SetVisibility(Message.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
}
