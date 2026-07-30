#pragma once

#include "CoreMinimal.h"
#include "Automata/Capture/CellRasterMode.h"
#include "Automata/Capture/SliceTileMode.h"
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

	/** Достроить снимок до бесшовного тайла отражением - см. ESliceTileMode.
	 *
	 *  Учтите, что тайл почти вдвое больше по каждой стороне, то есть вчетверо
	 *  по площади: предел MaxCapturePixels проверяется по ИСХОДНОМУ снимку, до
	 *  отражения, поэтому при больших срезах его может потребоваться поднять. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture")
	ESliceTileMode TileMode = ESliceTileMode::None;

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

	/** Сколько кадров снимает серия (см.
	 *  AAutomataOrchestrator::StartSeriesCapture()). Первый кадр - текущее
	 *  состояние, остальные снимаются по ходу симуляции. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture|Series",
			  meta = (ClampMin = "1", UIMin = "2", UIMax = "200"))
	int32 SeriesFrameCount = 12;

	/** Через сколько поколений снимать очередной кадр серии.
	 *
	 *  Считается в ПОКОЛЕНИЯХ, а не в кадрах экрана и не в секундах: узор
	 *  меняется поколениями, и только такой шаг даёт равномерную серию
	 *  независимо от того, с какой скоростью идёт симуляция и сколько
	 *  поколений посчиталось за один фоновый заход. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture|Series",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "100"))
	int32 SeriesGenerationsPerFrame = 5;

	/** Считать серию на максимальной скорости, не рисуя промежуточные
	 *  поколения на экране.
	 *
	 *  Смысл в том, что рендер клеток - самая дорогая часть кадра (AddInstances
	 *  это порядка 97% времени отрисовки), а серия его не использует вовсе:
	 *  снимок растеризуется прямо из сетки, и поколению незачем попадать на
	 *  экран, чтобы попасть в файл. Заодно снимается и пауза между шагами:
	 *  Speed задаёт темп для просмотра, а серии он только мешает.
	 *
	 *  Пока серия идёт, картинка на экране стоит - это нормально, прогресс
	 *  виден в строке состояния; по окончании экран догоняет одним рендером.
	 *  Выключите, если хотите наблюдать процесс глазами. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Capture|Series")
	bool bSeriesFastMode = true;
};
