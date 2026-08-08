#pragma once

#include "CoreMinimal.h"
#include "Automata/Grid/CellShape.h"
#include "RulePresets.generated.h"

/** Один готовый пресет правила для выпадашки в HUD (см.
 *  AAutomataOrchestrator::GetRulePresets()/ApplyRulePreset()).
 *
 *  USTRUCT(BlueprintType) с BlueprintReadOnly-полями - ровно тот же идиом,
 *  что у сводок HUD/FCellRenderStats: таблицу читает UMG-виджет, а не только
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

	/** Правда, если правило имеет смысл только на одной определённой решётке, и
	 *  ApplyRulePreset() обязан её включить (см. RequiredCellShape).
	 *
	 *  ПОЧЕМУ ФЛАГ, А НЕ ПРОСТО ПОЛЕ ФОРМЫ С ДЕФОЛТОМ "КУБ". Гонять одно правило
	 *  на разных решётках - это ровно то, ради чего форма клетки вообще
	 *  появилась, и пресет, который молча возвращает куб, отнимал бы эту
	 *  возможность у всех тринадцати правил каталога сразу. По умолчанию пресет
	 *  правила решётку НЕ трогает; флаг поднят лишь там, где иначе применилось
	 *  бы не то правило, которое написано в строке. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rules")
	bool bRequiresCellShape = false;

	/** Решётка, которую правило требует, - значима только при
	 *  bRequiresCellShape. Устойчивое имя, а не индекс в таблице форм: строки
	 *  той таблицы вправе переставляться. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rules")
	ECellShape RequiredCellShape = ECellShape::Cube;
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