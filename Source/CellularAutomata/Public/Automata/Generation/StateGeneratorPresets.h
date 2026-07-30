#pragma once

#include "CoreMinimal.h"
#include "Automata/Generation/StateGeneratorParams.h"
#include "StateGeneratorPresets.generated.h"

/** Готовый набор параметров генератора для выпадашки в HUD - см.
 *  AAutomataOrchestrator::GetStateGeneratorPresets()/ApplyStateGeneratorPreset().
 *
 *  USTRUCT(BlueprintType) с BlueprintReadOnly-полями - тот же идиом, что
 *  FRulePreset/FRenderPreset: таблицу читает UMG-виджет, а менять её из
 *  Blueprint нельзя, это константы кода, а не состояние актора.
 *
 *  Пресет несёт ТОЛЬКО геометрию и НЕ несёт правило: правило и структура
 *  подбираются независимо, и пресет, тихо переписывающий BirthCounts, ломал бы
 *  ровно тот сценарий, ради которого всё делалось, - взять свою структуру и
 *  крутить под неё правило. Подсказка про подходящее правило живёт в
 *  Description как текст. */
USTRUCT(BlueprintType)
struct FStateGeneratorPreset
{
	GENERATED_BODY()

	/** Отображаемое имя. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Generation")
	FString Name;

	/** Семейство для группировки в списке ("Решётка", "Тела", "Шум",
	 *  "Затравки"). Именно поле-данные, а не второй уровень перечисления -
	 *  см. doc-comment EStateGeneratorType. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Generation")
	FString FamilyName;

	/** Чем эта структура интересна и с каким правилом её пробовать. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Generation")
	FString Description;

	/** Готовые параметры - целиком, включая тип генератора. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Generation")
	FStateGeneratorParams Params;
};

/** Таблица готовых генераторов - плайн-namespace, как RulePresets/RenderPresets
 *  (не UObject: константные данные, ничего не хранит и не мутирует). */
namespace StateGeneratorPresets
{
	/** Все пресеты в порядке отображения. Ссылка на статическую таблицу,
	 *  строится один раз при первом обращении - вызывается на построение
	 *  выпадашки, не в горячем цикле. */
	CELLULARAUTOMATA_API const TArray<FStateGeneratorPreset>& GetAll();
}
