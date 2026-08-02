#pragma once

#include "CoreMinimal.h"
#include "Automata/Simulation/Neighborhood.h"

/** Разбор строковой нотации правила в формате "Survival/Birth/States/
 *  Neighborhood" (например "0-6/1,3/2/VN" или "13-26/13-14,17-19/2/M") -
 *  ровно тот формат, что использует сайт williamyang98/3D-Cellular-Automata
 *  (на основе Softology): Survival/Birth - списки через запятую, каждый
 *  элемент либо одно число, либо включительный диапазон "x-y"; States -
 *  общее число состояний клетки (>= 2, см. AAutomataOrchestrator::States);
 *  Neighborhood - имя набора оболочек, регистронезависимо, короткой или
 *  длинной формой: "M"/"Moore", "VN"/"VonNeumann", "VN2"/"VonNeumann2",
 *  "E"/"Edges", "C"/"Corners", "FA"/"FarAxes", "FE"/"FacesEdges" и так далее
 *  (полный список - таблица NeighborhoodTokens в .cpp, она же служит текстом
 *  ошибки, чтобы не разъехаться). Никакого отдельного радиуса в строке нет -
 *  см. Neighborhood.h о том, почему он убран; "VN2" теперь просто имя, а не
 *  "VN с цифрой".
 *
 *  ВАЖНО: это НЕ отмена решения "никакого строкового формата и парсинга" из
 *  doc-comment'а FCellularAutomatonRule - та формулировка была конкретно
 *  про классическую Conway-нотацию ("B3/S23"), которая ломается на
 *  двузначных счётчиках соседей (актуально для Moore - до 26 соседей в 3D,
 *  один символ на счётчик не работает). Формат здесь использует явные
 *  разделители (","/"-") и однозначен при любом числе цифр - независимая,
 *  отдельно обоснованная нотация, а не откат старой. См. также
 *  AAutomataOrchestrator::ApplyRuleString() - единственный потребитель этого
 *  парсера, применяющий результат к BirthCounts/SurvivalCounts/States/
 *  Neighborhood по имени поля (никогда позиционно - порядок полей в
 *  строке, Survival затем Birth, не совпадает с порядком объявления тех
 *  UPROPERTY). */
namespace RuleStringParser
{
	/** Результат успешного разбора - см. doc-comment ParseRuleString(). */
	struct FParsedRule
	{
		TArray<int32> SurvivalCounts;
		TArray<int32> BirthCounts;
		int32 States = 2;
		ENeighborhood Neighborhood = ENeighborhood::Moore;
	};

	/** Возвращает true и заполняет OutResult при успешном разборе; иначе
	 *  возвращает false и заполняет OutError описанием проблемы, не трогая
	 *  OutResult - вызывающий код не должен использовать частично
	 *  заполненный результат при false (см. AAutomataOrchestrator::
	 *  ApplyRuleString() - "никогда не применять частично"). */
	CELLULARAUTOMATA_API bool ParseRuleString(const FString& RuleString, FParsedRule& OutResult, FString& OutError);

	/** Обратная операция к ParseRuleString(): собирает строку правила из
	 *  текущих BirthCounts/SurvivalCounts/States/Neighborhood. Нужна HUD'у
	 *  (AAutomataOrchestrator::GetActiveRuleString()), чтобы показывать
	 *  ДЕЙСТВУЮЩЕЕ правило, а не поле RuleString - то могло быть не применено,
	 *  пустое (правило собрали массивами в Details panel) или устареть после
	 *  ручной правки массивов.
	 *
	 *  Списки счётчиков сортируются, дедуплицируются и сжимаются обратно в
	 *  диапазоны ("1,2,3,7" -> "1-3,7"), так что для строки, полученной из
	 *  ParseRuleString(), результат совпадает с исходной с точностью до
	 *  нормализации порядка/формы диапазонов. */
	CELLULARAUTOMATA_API FString FormatRuleString(const TArray<int32>& SurvivalCounts, const TArray<int32>& BirthCounts, int32 States, ENeighborhood Neighborhood);
}
