#pragma once

#include "CoreMinimal.h"
#include "Automata/Sonification/SonificationParams.h"
#include "SonificationPresets.generated.h"

/** Один готовый набор настроек сонификации (см.
 *  AAutomataOrchestrator::ApplySonificationPreset(), хоткей Shift+P).
 *
 *  Тот же идиом, что FCapturePreset/FRenderPreset/FRulePreset: константы кода,
 *  USTRUCT(BlueprintType) с BlueprintReadOnly-полями, чтобы таблицу читал и
 *  UMG-виджет, но менять её из Blueprint было нельзя.
 *
 *  Зачем поверх и без того редактируемой в панели FSonificationParams: связка
 *  здесь не в одном поле, а в согласовании окна и постоянных времени. Окно в
 *  шестнадцать поколений со сглаживанием в две секунды - не "быстро" и не
 *  "медленно", а бессмыслица: измерение дрожит, а звук об этом не узнаёт.
 *  Руками это каждый раз шесть-семь полей в согласованных значениях, и ошибка
 *  в одном тихо превращает прибор в шум.
 *
 *  Ссылок на звуковые ассеты здесь НЕТ и быть не может. Пресет - константа
 *  кода, а прямых ссылок из кода на .uasset в этом проекте нет принципиально
 *  (ровно поэтому UMainHudWidget ищет оркестратор через GetActorOfClass()).
 *  Тембр целиком живёт в графе MetaSound; пресет - только числа, которыми этот
 *  граф кормят. Разделение то же, что с HUD: C++ даёт данные, вёрстку делают
 *  в редакторе. */
USTRUCT(BlueprintType)
struct FSonificationPreset
{
	GENERATED_BODY()

	/** Отображаемое имя - оно же уходит в строку состояния при Shift+P. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Audio")
	FString Name;

	/** Короткое "зачем это нужно" - для подсказки в HUD. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Audio")
	FString Description;

	/** Настройки целиком - применяются присваиванием всей структуры, поэтому
	 *  результат никогда не зависит от того, какой набор стоял до этого. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Audio")
	FSonificationParams Params;
};

/** Таблица наборов - плайн-namespace, как RulePresets/RenderPresets/
 *  CapturePresets. */
namespace SonificationPresets
{
	/** Все наборы в порядке отображения; индекс - то, что принимает
	 *  ApplySonificationPreset() и что перебирает Shift+P. Ссылка на
	 *  статическую таблицу, строится один раз при первом обращении. */
	CELLULARAUTOMATA_API const TArray<FSonificationPreset>& GetAll();
}