#pragma once

#include "CoreMinimal.h"
#include "RulePresets.generated.h"

/** Один готовый пресет правила для выпадашки в HUD (см.
 *  AAutomataOrchestrator::GetRulePresets()/ApplyRulePreset()).
 *
 *  USTRUCT(BlueprintType) с BlueprintReadOnly-полями - ровно тот же идиом,
 *  что FHudStats/FCellRenderStats: таблицу читает UMG-виджет, а не только
 *  нативный код, и менять её из Blueprint нельзя (пресеты - константы кода,
 *  не состояние актора).
 *
 *  RuleString хранится строкой, а не уже разобранными BirthCounts/
 *  SurvivalCounts/States/Neighborhood, специально: ApplyRulePreset() гоняет
 *  её через тот же RuleStringParser::ParseRuleString(), что и ручной ввод в
 *  поле RuleString - один путь применения правила вместо двух, которые могли
 *  бы разъехаться в семантике. */
USTRUCT(BlueprintType)
struct FRulePreset
{
	GENERATED_BODY()

	/** Отображаемое имя - как на сайте-источнике (см. RulePresets::GetAll()). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rules")
	FString Name;

	/** Правило в нотации "Survival/Birth/States/Neighborhood" - см.
	 *  RuleStringParser.h. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rules")
	FString RuleString;

	/** Рекомендуемый радиус стартового шара (в клетках) - в
	 *  AAutomataOrchestrator::SpawnRadius. См. комментарий к таблице в
	 *  RulePresets.cpp про то, откуда взяты эти числа. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rules")
	int32 SpawnRadius = 0;

	/** Рекомендуемое число стартовых клеток - в
	 *  AAutomataOrchestrator::Amount. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rules")
	int32 Amount = 0;
};

/** Таблица готовых правил - плайн-namespace, как CellAging/CellSelection/
 *  RuleStringParser (не UObject: это константные данные, ничего не хранит и
 *  не мутирует). */
namespace RulePresets
{
	/** Все пресеты в порядке отображения. Ссылка на статическую таблицу,
	 *  строится один раз при первом обращении - вызывается на построение
	 *  выпадашки в HUD, не в горячем цикле. */
	CELLULARAUTOMATA_API const TArray<FRulePreset>& GetAll();
}