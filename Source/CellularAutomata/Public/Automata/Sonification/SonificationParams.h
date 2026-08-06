#pragma once

#include "CoreMinimal.h"
#include "SonificationParams.generated.h"

/** Всё, чем настраивается перевод статистики симуляции в параметры звука:
 *  окно измерения, масштабы нормировки, постоянные сглаживания и пороги
 *  классификации формы.
 *
 *  Одной структурой, а не полями россыпью по актору - тот же идиом, что
 *  FSliceCaptureParams: набор осмыслен только целиком (окно в 16 поколений с
 *  постоянной времени в 2 секунды - это не "быстро" и не "медленно", это
 *  бессмыслица), и пресеты применяются присваиванием всей структуры, поэтому
 *  хвостов от предыдущего набора не остаётся по построению.
 *
 *  Здесь НЕТ и не может быть ссылок на звуковые ассеты. Пресеты - константы
 *  кода, а тембр живёт в графе MetaSound; C++ отвечает только за числа, которыми
 *  этот граф кормят. */
USTRUCT(BlueprintType)
struct FSonificationParams
{
	GENERATED_BODY()

	// ---------------------------------------------------------------- окно

	/** Ширина окна измерения В ПОКОЛЕНИЯХ, а не в замерах.
	 *
	 *  Именно в поколениях, потому что замеры стоят неравномерно: при
	 *  StepsPerRender 256 в шестьдесят четыре поколения не попадает ни одного
	 *  замера вовсе. Окно, заданное по значению X, переживает дыры; окно,
	 *  заданное по количеству точек, растягивалось и сжималось бы при каждой
	 *  смене размера пачки - ровно та беда, из-за которой FGenerationSample
	 *  хранит пару, а не одно значение. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "2", UIMin = "8", UIMax = "512"))
	int32 WindowGenerations = 64;

	/** Пол по числу замеров: если в WindowGenerations не набралось столько
	 *  точек, окно расширяется назад, сколько бы поколений оно ни накрыло.
	 *  Без этого на больших StepsPerRender измерять было бы нечего вообще. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "2", UIMin = "2", UIMax = "64"))
	int32 MinWindowSamples = 8;

	/** Сколько замеров в окне считается полным доверием (Confidence01 = 1).
	 *  Меньше - величины считаются, но графу сообщается, что верить им можно не
	 *  вполне: колебания внутри пропущенных поколений не записаны нигде, и
	 *  осцилляционный слой надо приглушать, а не выдавать шум за музыку. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "2", UIMin = "4", UIMax = "64"))
	int32 MinReliableSamples = 16;

	// ----------------------------------------------------------- масштабы

	/** Население, дающее Population01 == 1. Шкала логарифмическая
	 *  (ln(1+N)/ln(1+FullScalePopulation)), иначе на семи миллионах клеток
	 *  весь интересный диапазон схлопнулся бы в первые проценты фейдера. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "10", UIMin = "1000", UIMax = "10000000"))
	int32 FullScalePopulation = 1000000;

	/** Наклон, дающий |Slope| == 1, в е-фолдах на поколение.
	 *
	 *  0.35 - это примерно "в полтора раза за поколение". Для ориентира:
	 *  0.693 = удвоение за поколение, 0.0069 = удвоение за сотню. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.001", UIMin = "0.01", UIMax = "1.0"))
	float SlopeFullScale = 0.35f;

	/** Длина пути кривой на поколение, дающая Activity == 1. Это и есть
	 *  "плотность шуршания": сколько всего движения было, независимо от того,
	 *  куда оно в итоге пришло. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.001", UIMin = "0.01", UIMax = "1.0"))
	float ActivityFullScale = 0.35f;

	/** Поколений в секунду, дающих Rate == 1 - темп шуршания. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.1", UIMin = "1.0", UIMax = "240.0"))
	float RateFullScale = 60.0f;

	/** Размерность структуры, дающая Dimension == 1. Три - плотное тело;
	 *  около двух - оболочка, около единицы - нить. Величину считает
	 *  GenerationHistory::FitGrowthExponent(), своей математики тут нет. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.5", UIMin = "1.0", UIMax = "4.0"))
	float DimensionFullScale = 3.0f;

	// -------------------------------------------------------- сглаживание

	/** Постоянные времени экспоненциального сглаживания, В СЕКУНДАХ.
	 *
	 *  Разные по величинам намеренно: население должно откликаться почти сразу,
	 *  а размерность структуры - медленная глобальная характеристика, и дёрганый
	 *  тембр от неё был бы враньём. Секунды, а не кадры, потому что кадр здесь
	 *  плавает от восьми миллисекунд до секунды с лишним. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "5.0"))
	float TauPopulation = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "5.0"))
	float TauSlope = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "5.0"))
	float TauCurvature = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10.0"))
	float TauDimension = 1.5f;

	/** Потолок шага сглаживания.
	 *
	 *  Кадр может выйти длинным - AddInstances() на сотнях тысяч клеток стоит
	 *  десятки и сотни миллисекунд, и это синхронно на игровом потоке. Без
	 *  подрезки один такой кадр протащил бы все величины к цели рывком, то есть
	 *  ровно тем щелчком, ради устранения которого сглаживание и заведено. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.01", UIMin = "0.05", UIMax = "1.0"))
	float MaxSmoothStep = 0.25f;

	/** Постоянная времени не опускается ниже половины ожидаемого интервала
	 *  между замерами. На Speed = 0.5 замеры приходят раз в две секунды, и
	 *  сглаживание в 0.35 с превратило бы кривую в лестницу из ступенек. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio")
	bool bAdaptTauToStepRate = true;

	// ------------------------------------------------------------ простой

	/** Сколько секунд без новых поколений ещё считается работой (Liveness = 1).
	 *
	 *  Берётся не меньше двух ожидаемых интервалов между замерами: на медленной
	 *  скорости пауза между поколениями - это норма, а не простой. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10.0"))
	float StaleGraceSeconds = 1.0f;

	/** За сколько секунд после StaleGraceSeconds Liveness уезжает в ноль. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.01", UIMin = "0.1", UIMax = "10.0"))
	float StaleFadeSeconds = 1.5f;

	// -------------------------------------------------------------- пороги

	/** Ниже этого |наклона| населением считается, что оно держится. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "0.2"))
	float SteadySlope = 0.01f;

	/** Наклон круче этого (вниз) - уже не спад, а обвал. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.01", UIMin = "0.05", UIMax = "2.0"))
	float CollapseSlope = 0.3f;

	/** Изгиб, за которым рост считается взрывным (вверх) или выдыхающимся
	 *  (вниз). Величина безразмерная, в [-1, 1], - см.
	 *  FSonificationFeatures::Bend.
	 *
	 *  Абсолютный порог на кривизну здесь не работает в принципе, и это не
	 *  вопрос подбора числа: кривизна масштабируется как КВАДРАТ наклона (у
	 *  степенного роста n^d наклон d/n, кривизна -d/n^2), так что любое
	 *  фиксированное значение либо срабатывает всегда на быстрых кривых, либо
	 *  никогда на медленных. Отношение изменения наклона к самому наклону от
	 *  масштаба не зависит вовсе. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.05", UIMax = "0.6"))
	float BendThreshold = 0.15f;

	/** Доля пути, не превратившаяся в перемещение, за которой движение
	 *  считается колебанием. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.5", UIMax = "1.0"))
	float OscillationThreshold = 0.7f;

	/** Ниже этой размерности растущая структура считается выдыхающейся, а не
	 *  линейно растущей.
	 *
	 *  Существует ровно ради одной ловушки: в лог-координатах линейный рост
	 *  НЕОТЛИЧИМ от лёгкого насыщения. У N = a*n наклон dy/dx = 1/n -
	 *  положительный, убывающий, с отрицательной кривизной, то есть ровно тот
	 *  же признак, что у выходящей на плато кривой. Различает их только
	 *  показатель роста: линейный рост даёт d около единицы, насыщение - d,
	 *  уползающее к нулю. Поэтому классификация обязана смотреть на обе
	 *  величины, и на это есть отдельный автотест.
	 *
	 *  Значение по умолчанию чуть ниже единицы не случайно: степенной рост с
	 *  показателем d >= 1 - это рост, а прогиб НИЖЕ линейного - уже насыщение.
	 *  Линейный рост даёт ровно 1.0 (подгонка точная), выдыхающийся - заметно
	 *  меньше, так что запас есть в обе стороны. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Audio",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "2.0"))
	float SaturationDimension = 0.9f;
};