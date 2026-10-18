#pragma once

#include "CoreMinimal.h"
#include "Types/SlateEnums.h"
#include "Ui/HudElementWidget.h"
#include "Ui/HudPresetKind.h"
#include "HudPresetSelector.generated.h"

class UComboBoxString;

/**
 * Выпадающий список пресетов в HUD (WBP_HudPresetSelector). Как и прочие
 * элементы HUD v2, один виджет на все таблицы: что он перечисляет, решает
 * поле Kind экземпляра.
 *
 * Список заполняется из GetXPresets() оркестратора при конструировании, выбор
 * применяется ApplyXPreset(Index). Применяется только выбор ПОЛЬЗОВАТЕЛЯ:
 * программная установка выделения (синхронизация, ESelectInfo::Direct) пресет
 * не применяет - иначе синхронизация с хоткеем тут же применила бы пресет
 * второй раз и, для правила, сбросила бы ручную правку массивов.
 *
 * Синхронизация с тем, что переключено мимо списка (хоткеями F1-F4, вводом
 * правила), есть там, где текущее значение вообще известно: у профиля рендера
 * по RenderPresetIndex, у правила - по совпадению строки правила с пресетом.
 * У остальных таблиц "текущего пресета" в оркестраторе нет, список помнит
 * последний выбор.
 */
UCLASS(Abstract)
class CELLULARAUTOMATA_API UHudPresetSelector : public UHudElementWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD")
	EHudPresetKind Kind = EHudPresetKind::Rule;

	/** Генератор: применить пресет и сразу сгенерировать состояние. Без этого
	 *  выбор генератора ничего не меняет на экране до следующего N. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|HUD")
	bool bGenerateOnSelect = true;

	/** Перечитать список из оркестратора - на случай, если таблица изменилась. */
	UFUNCTION(BlueprintCallable, Category = "Automata|HUD")
	void RefreshOptions();

protected:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Имя дочернего виджета в WBP обязано быть "Combo". Optional, чтобы WBP
	 *  компилировался и до того, как список в него положили. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> Combo;

private:
	UFUNCTION()
	void HandleSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	/** Индекс пресета, который сейчас действует, или INDEX_NONE, если это
	 *  неизвестно (см. комментарий к классу). */
	int32 FindCurrentIndex();

	void Apply(int32 Index);
};
