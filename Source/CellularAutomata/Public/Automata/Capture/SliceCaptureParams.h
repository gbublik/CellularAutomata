#pragma once

#include "CoreMinimal.h"
#include "Automata/Capture/CellRasterMode.h"
#include "SliceCaptureParams.generated.h"

/** Настройки съёмки текстурного среза - AAutomataOrchestrator::SliceCaptureParams.
 *
 *  Одна структура, а не поля россыпью на акторе: то же решение и по тем же
 *  причинам, что у FStateGeneratorParams (EditCondition резолвится внутри
 *  структуры, HUD читает и пишет её целиком, а в сейв она поедет одной
 *  строкой, если однажды понадобится). */
USTRUCT(BlueprintType)
struct FSliceCaptureParams
{
	GENERATED_BODY()

	/** Сколько пикселей занимает клетка по стороне. Блок NxN заливается одним
	 *  цветом: масштабирование здесь - копирование, а не фильтрация, поэтому
	 *  границы клеток попадают точно на пиксели при любом значении. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "32"))
	int32 PixelsPerCell = 1;

	/** Что делать с клетками, попавшими в один пиксель - см. ECellRasterMode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture")
	ECellRasterMode Mode = ECellRasterMode::NearestToCamera;

	/** Прозрачный фон вместо цвета фона - снимок сразу годится как накладка
	 *  без обтравки. Выключено по умолчанию: непрозрачный PNG предсказуемее в
	 *  чужих пайплайнах, а как маску удобнее читать яркость. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture")
	bool bTransparentBackground = false;

	/** Цвет пустых мест. Белый фон для орнамента встречается не реже чёрного,
	 *  поэтому поле, а не константа. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture",
			  meta = (EditCondition = "!bTransparentBackground"))
	FColor BackgroundColor = FColor(0, 0, 0, 255);

	/** Цвет клеток в режиме Silhouette. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture",
			  meta = (EditCondition = "Mode == ECellRasterMode::Silhouette"))
	FColor ForegroundColor = FColor::White;

	/** Кодировать цвета в sRGB.
	 *
	 *  Включено по умолчанию, и это ВАЖНО: рампа отдаёт линейные байты (они
	 *  нужны материалу, см. FCellRenderInstance), а PNG любой просмотрщик
	 *  читает как sRGB - без этой галки снимок выходит заметно темнее того,
	 *  что на экране, и ничего об этом не сообщает. Выключать стоит, только
	 *  если срез идёт обратно в движок как маска с отключённым sRGB на
	 *  текстуре. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture")
	bool bEncodeSRGB = true;
};
