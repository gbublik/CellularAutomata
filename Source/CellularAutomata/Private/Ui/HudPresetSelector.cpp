#include "Ui/HudPresetSelector.h"
#include "Components/ComboBoxString.h"
#include "Orchestration/AutomataOrchestrator.h"

namespace
{
	template <typename TPreset>
	TArray<FString> NamesOf(const TArray<TPreset>& Presets)
	{
		TArray<FString> Names;
		Names.Reserve(Presets.Num());
		for (const TPreset& Preset : Presets)
		{
			Names.Add(Preset.Name);
		}
		return Names;
	}

	TArray<FString> PresetNames(AAutomataOrchestrator& Orchestrator, EHudPresetKind Kind)
	{
		switch (Kind)
		{
		case EHudPresetKind::Rule:      return NamesOf(Orchestrator.GetRulePresets());
		case EHudPresetKind::CellShape: return NamesOf(Orchestrator.GetCellShapePresets());
		case EHudPresetKind::Generator: return NamesOf(Orchestrator.GetStateGeneratorPresets());
		case EHudPresetKind::Render:    return NamesOf(Orchestrator.GetRenderPresets());
		case EHudPresetKind::Light:     return NamesOf(Orchestrator.GetLightPresets());
		case EHudPresetKind::Sound:     return NamesOf(Orchestrator.GetSonificationPresets());
		}
		return {};
	}
}

void UHudPresetSelector::NativeConstruct()
{
	Super::NativeConstruct();

	if (Combo)
	{
		Combo->OnSelectionChanged.AddUniqueDynamic(this, &UHudPresetSelector::HandleSelectionChanged);
	}
	RefreshOptions();
}

void UHudPresetSelector::RefreshOptions()
{
	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	if (!Combo || !Orchestrator)
	{
		return;
	}

	Combo->ClearOptions();
	for (const FString& Name : PresetNames(*Orchestrator, Kind))
	{
		Combo->AddOption(Name);
	}

	const int32 Current = FindCurrentIndex();
	if (Current != INDEX_NONE)
	{
		// Direct - и потому НЕ применяется, см. HandleSelectionChanged().
		Combo->SetSelectedIndex(Current);
	}
}

void UHudPresetSelector::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Пока список открыт, не трогаем выделение под рукой у пользователя.
	if (!Combo || Combo->IsOpen())
	{
		return;
	}

	const int32 Current = FindCurrentIndex();
	if (Current != INDEX_NONE)
	{
		if (Current != Combo->GetSelectedIndex())
		{
			Combo->SetSelectedIndex(Current);
		}
	}
	else if (Kind != EHudPresetKind::Generator && Combo->GetSelectedIndex() != INDEX_NONE)
	{
		// Текущее значение известно, но ни с одним пресетом не совпадает
		// (правило введено руками, форма задана полями) - старый пресет в
		// списке был бы враньём. У генератора "неизвестно" значит другое (см.
		// FindCurrentIndex()), там выбор пользователя остаётся.
		Combo->ClearSelection();
	}
}

int32 UHudPresetSelector::FindCurrentIndex()
{
	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	if (!Orchestrator)
	{
		return INDEX_NONE;
	}

	switch (Kind)
	{
	case EHudPresetKind::Render:
		return Orchestrator->GetHudRenderStats().RenderPresetIndex;

	case EHudPresetKind::Rule:
	{
		// Строка правила, которое считается СЕЙЧАС (а не поле ввода) - тот же
		// источник, что у HUD-строки RULE.
		const FString Active = Orchestrator->GetActiveRuleString();
		const TArray<FRulePreset> Presets = Orchestrator->GetRulePresets();
		for (int32 Index = 0; Index < Presets.Num(); ++Index)
		{
			if (Presets[Index].RuleString.Equals(Active, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	case EHudPresetKind::CellShape:
	{
		// CellShape - "какая форма применена последней" (см. его комментарий
		// в оркестраторе); таблица форм замкнута, у каждой своя ECellShape.
		const TArray<FCellShapePreset> Presets = Orchestrator->GetCellShapePresets();
		for (int32 Index = 0; Index < Presets.Num(); ++Index)
		{
			if (Presets[Index].Shape == Orchestrator->CellShape)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	case EHudPresetKind::Light:
		return Orchestrator->ActiveLightPresetIndex;

	case EHudPresetKind::Sound:
	{
		const FString Active = Orchestrator->GetActiveSonificationPresetName();
		const TArray<FSonificationPreset> Presets = Orchestrator->GetSonificationPresets();
		for (int32 Index = 0; Index < Presets.Num(); ++Index)
		{
			if (!Active.IsEmpty() && Presets[Index].Name == Active)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	case EHudPresetKind::Generator:
	{
		// Пресетов генератора несколько на один тип (разные параметры одного
		// семейства), а "какой пресет применён" оркестратор не хранит. По
		// типу узнаётся только тот, кто в таблице единственный своего типа;
		// для остальных список помнит последний выбор пользователя.
		const EStateGeneratorType ActiveType = Orchestrator->GetStateGeneratorParams().Type;
		const TArray<FStateGeneratorPreset> Presets = Orchestrator->GetStateGeneratorPresets();
		int32 Found = INDEX_NONE;
		for (int32 Index = 0; Index < Presets.Num(); ++Index)
		{
			if (Presets[Index].Params.Type == ActiveType)
			{
				if (Found != INDEX_NONE)
				{
					return INDEX_NONE;
				}
				Found = Index;
			}
		}
		return Found;
	}
	}
	return INDEX_NONE;
}

void UHudPresetSelector::HandleSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	// Программная установка (синхронизация, RefreshOptions) - не выбор
	// пользователя, применять нечего.
	if (SelectionType == ESelectInfo::Direct || !Combo)
	{
		return;
	}
	Apply(Combo->FindOptionIndex(SelectedItem));
}

void UHudPresetSelector::Apply(int32 Index)
{
	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	if (!Orchestrator || Index == INDEX_NONE)
	{
		return;
	}

	switch (Kind)
	{
	case EHudPresetKind::Rule:      Orchestrator->ApplyRulePreset(Index); break;
	case EHudPresetKind::CellShape: Orchestrator->ApplyCellShapePreset(Index); break;
	case EHudPresetKind::Generator: Orchestrator->ApplyStateGeneratorPreset(Index, bGenerateOnSelect); break;
	case EHudPresetKind::Render:    Orchestrator->ApplyRenderPreset(Index); break;
	case EHudPresetKind::Light:     Orchestrator->ApplyLightPreset(Index); break;
	case EHudPresetKind::Sound:     Orchestrator->ApplySonificationPreset(Index); break;
	}
}
