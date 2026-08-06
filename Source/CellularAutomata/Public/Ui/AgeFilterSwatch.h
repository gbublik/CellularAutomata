#pragma once

#include "CoreMinimal.h"
#include "AgeFilterSwatch.generated.h"

/** Один квадратик легенды фильтра по возрасту - то, чем HUD показывает, каким
 *  цветом нарисован слой и виден ли он сейчас.
 *
 *  Смысл именно в ЛЕГЕНДЕ, а не в украшении: цвет здесь тот же самый, которым
 *  клетки этого возраста нарисованы на экране (та же рампа AgeColors, тот же
 *  SampleColorRamp()), поэтому по ряду квадратиков можно прочитать картинку.
 *  Нарисовать вместо этого ровный градиент было бы красивее и бесполезно -
 *  он не соответствовал бы ничему на экране.
 *
 *  Зачем вообще: всплывающее сообщение о фильтре (ShowStatusMessage() в
 *  ApplyAgeFilterChange()) показывается в момент нажатия и гаснет, а
 *  AliveCellCount на HUD считает ЖИВЫХ - при включённом фильтре число и
 *  картинка расходятся, и объяснить это расхождение постоянно видимой
 *  подсказкой больше нечем. У соседних отсечений такие индикаторы уже есть
 *  (bCullVolumeActive, bViewSliceActive), у фильтра возраста не было. */
USTRUCT(BlueprintType)
struct FAgeFilterSwatch
{
	GENERATED_BODY()

	/** Возраст, который показывает эта клавиша. Внутренняя нумерация от нуля:
	 *  0 - только что родившиеся. Клавиша при этом другая, см. KeyLabel. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	int32 Age = 0;

	/** Подпись клавиши - "1".."9", "0" при раскладке по умолчанию.
	 *
	 *  Берётся из HotkeyRegistry, а не пишется в вёрстке руками: клавиши
	 *  настраиваются из Config/DefaultInput.ini, и зашитая в UMG подпись стала
	 *  бы ложью при первом же переназначении. Пустая, если контроллер ещё не
	 *  готов (до BeginPlay/вне PIE). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	FString KeyLabel;

	/** Цвет клеток этого возраста - ровно то, что на экране. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	FLinearColor Color = FLinearColor::White;

	/** Дальний конец диапазона, который покрывает квадратик. У всех, кроме
	 *  последнего, совпадает с Color.
	 *
	 *  Существует ради последнего: он покрывает не один возраст, а всё от Age
	 *  и старше, то есть 9..255 - почти всю рампу. Одним цветом такой квадратик
	 *  был бы враньём, поэтому его стоит рисовать градиентом Color -> ColorEnd
	 *  (или как-то ещё помечать, см. bIncludesOlder). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	FLinearColor ColorEnd = FLinearColor::White;

	/** Виден ли этот слой сейчас. Когда фильтр снят - true у всех. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bVisible = true;

	/** Квадратик покрывает не один возраст, а весь хвост от Age и старше. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|HUD")
	bool bIncludesOlder = false;
};
