#pragma once

#include "CoreMinimal.h"
#include "Automata/Capture/SliceCaptureParams.h"
#include "CapturePresets.generated.h"

/** Один готовый набор настроек съёмки (см.
 *  AAutomataOrchestrator::GetCapturePresets()/ApplyCapturePreset(), хоткей
 *  Shift+F7).
 *
 *  USTRUCT(BlueprintType) с BlueprintReadOnly-полями - тот же идиом, что
 *  FRulePreset/FRenderPreset: таблицу читает UMG-виджет, а не только нативный
 *  код, и менять её из Blueprint нельзя (пресеты - константы кода, не
 *  состояние актора).
 *
 *  Зачем это поверх и без того редактируемой в панели FSliceCaptureParams:
 *  осмысленные наборы здесь - не "одно поле покрутить", а связки из четырёх-
 *  пяти полей сразу (режим растеризации, плитка, масштаб, длина серии), причём
 *  связки взаимно несовместимые. Перебор кандидатов хочет мелкий силуэт частыми
 *  кадрами, финальный экспорт - крупный цветной снимок без серии вовсе. Руками
 *  это каждый раз пять полей в правильных значениях, и ошибка в одном тихо
 *  портит весь заход: серия из шестидесяти кадров по 8 пикселей на клетку
 *  считается долго и занимает место, а понятно это становится в конце.
 *
 *  Пресет хранит FSliceCaptureParams ЦЕЛИКОМ, а не список отличий от текущих
 *  настроек, и применяется присваиванием всей структуры - ровно та же
 *  детерминированность, что у FRenderPreset: результат никогда не зависит от
 *  того, какой пресет применяли до этого, и восстанавливать после него ничего
 *  не нужно. Поэтому здесь нет и не может быть "это поле не трогать". */
USTRUCT(BlueprintType)
struct FCapturePreset
{
	GENERATED_BODY()

	/** Отображаемое имя - оно же уходит в строку состояния при Shift+F7. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Capture")
	FString Name;

	/** Короткое "зачем это нужно" - для подсказки в HUD. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Capture")
	FString Description;

	/** Настройки целиком - см. doc-comment структуры про то, почему целиком. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Capture")
	FSliceCaptureParams Params;
};

/** Таблица наборов настроек съёмки - плайн-namespace, как RulePresets/
 *  RenderPresets/CellAging (не UObject: константные данные, ничего не хранит и
 *  не мутирует). */
namespace CapturePresets
{
	/** Все наборы в порядке отображения; индекс в этом массиве - то, что
	 *  принимает ApplyCapturePreset() и что перебирает Shift+F7. Ссылка на
	 *  статическую таблицу, строится один раз при первом обращении. */
	CELLULARAUTOMATA_API const TArray<FCapturePreset>& GetAll();
}