#include "Automata/Sonification/SonificationCurve.h"

namespace
{
	/** y-координата замера: ln(1 + N).
	 *
	 *  Единица под логарифмом - не защитная заглушка, а несущая часть: вымершая
	 *  сетка обязана остаться в области определения, потому что это самое
	 *  интересное событие всей симуляции, а не пропуск в данных. ln(1+0) = 0,
	 *  никаких минус бесконечностей, и при больших N поправка порядка 1/N. */
	FORCEINLINE double LogPopulation(int32 AliveCount)
	{
		return FMath::Loge(1.0 + static_cast<double>(FMath::Max(AliveCount, 0)));
	}
}

int32 SonificationCurve::FindWindowStart(const TArray<FGenerationSample>& History,
	int64 WindowGenerations, int32 MinSamples)
{
	const int32 Num = History.Num();
	if (Num == 0)
	{
		return INDEX_NONE;
	}

	const int64 LastGeneration = History.Last().Generation;
	const int64 Window = FMath::Max<int64>(WindowGenerations, 1);
	const int64 Cutoff = LastGeneration - Window;

	// Назад, пока предыдущий замер ещё внутри окна по НОМЕРУ ПОКОЛЕНИЯ. Массив
	// отсортирован по Generation (Append() только дописывает в конец), но
	// бинарного поиска тут не надо: окно - десятки замеров, а зовётся это раз в
	// обновление звука, а не на каждую клетку.
	int32 First = Num - 1;
	while (First > 0 && History[First - 1].Generation >= Cutoff)
	{
		--First;
	}

	// Пол по числу замеров. Без него при StepsPerRender 256 в окно из
	// шестидесяти четырёх поколений не попадёт ни одного замера, и мерить будет
	// нечего вовсе - хотя данные есть, просто стоят реже окна.
	const int32 Floor = FMath::Max(MinSamples, 2);
	if (Num - First < Floor)
	{
		First = FMath::Max(0, Num - Floor);
	}

	return First;
}

bool SonificationCurve::FitLogSlope(const TArray<FGenerationSample>& History,
	int32 First, int32 Last, double& OutSlope, double& OutCentroidX)
{
	OutSlope = 0.0;
	OutCentroidX = 0.0;

	if (!History.IsValidIndex(First) || !History.IsValidIndex(Last) || Last - First < 1)
	{
		return false;
	}

	const int32 Count = Last - First + 1;

	double SumX = 0.0;
	double SumY = 0.0;
	for (int32 Index = First; Index <= Last; ++Index)
	{
		SumX += static_cast<double>(History[Index].Generation);
		SumY += LogPopulation(History[Index].AliveCount);
	}

	const double MeanX = SumX / Count;
	const double MeanY = SumY / Count;
	OutCentroidX = MeanX;

	double Sxx = 0.0;
	double Sxy = 0.0;
	for (int32 Index = First; Index <= Last; ++Index)
	{
		const double dx = static_cast<double>(History[Index].Generation) - MeanX;
		Sxx += dx * dx;
		Sxy += dx * (LogPopulation(History[Index].AliveCount) - MeanY);
	}

	// Все замеры на одном поколении. Случай не теоретический: NoteRendered()
	// правит последний замер НА МЕСТЕ, а рендер дёргается на каждое движение
	// камеры при включённом срезе, так что на паузе окно вполне может
	// схлопнуться в одну точку по X.
	if (Sxx <= UE_DOUBLE_SMALL_NUMBER)
	{
		return false;
	}

	OutSlope = Sxy / Sxx;
	return true;
}

bool SonificationCurve::FitBend(const TArray<FGenerationSample>& History,
	int32 First, int32 Last, double& OutCurvature, double& OutBend)
{
	OutCurvature = 0.0;
	OutBend = 0.0;

	// Не меньше четырёх замеров: по два на половину. На трёх точках половины
	// выродились бы в две пары, и "изгиб" стал бы просто второй разностью - то
	// самое точное, но бессмысленное решение, из-за которого отвергнут и
	// квадратичный МНК.
	if (!History.IsValidIndex(First) || !History.IsValidIndex(Last) || Last - First < 3)
	{
		return false;
	}

	const int64 FirstGeneration = History[First].Generation;
	const int64 LastGeneration = History[Last].Generation;
	if (LastGeneration <= FirstGeneration)
	{
		return false;
	}

	// Делим по ЗНАЧЕНИЮ поколения, а не по середине массива: дыра в замерах
	// сдвинула бы середину массива в сторону и перекосила бы половины.
	const int64 MidGeneration = FirstGeneration + (LastGeneration - FirstGeneration) / 2;

	int32 Mid = First;
	while (Mid < Last && History[Mid].Generation < MidGeneration)
	{
		++Mid;
	}

	// Половины перекрываются на Mid - замер на границе честно принадлежит обеим,
	// и это заодно спасает узкие окна от вырождения одной из половин в точку.
	if (Mid - First < 1 || Last - Mid < 1)
	{
		return false;
	}

	double SlopeOld = 0.0;
	double CentroidOld = 0.0;
	double SlopeRecent = 0.0;
	double CentroidRecent = 0.0;
	if (!FitLogSlope(History, First, Mid, SlopeOld, CentroidOld)
		|| !FitLogSlope(History, Mid, Last, SlopeRecent, CentroidRecent))
	{
		return false;
	}

	const double CentroidSpan = CentroidRecent - CentroidOld;
	if (FMath::Abs(CentroidSpan) <= UE_DOUBLE_SMALL_NUMBER)
	{
		return false;
	}

	OutCurvature = (SlopeRecent - SlopeOld) / CentroidSpan;

	// Безразмерная форма: во сколько раз наклон изменился относительно самого
	// себя. Знаменатель - сумма модулей, а не модуль суммы, чтобы разворот
	// (наклон сменил знак) давал ровно -1, а не деление на околонуль.
	const double Scale = FMath::Abs(SlopeRecent) + FMath::Abs(SlopeOld);
	OutBend = (Scale > UE_DOUBLE_SMALL_NUMBER)
		? FMath::Clamp((SlopeRecent - SlopeOld) / Scale, -1.0, 1.0)
		: 0.0;

	return true;
}

ESonificationShape SonificationCurve::ClassifyShape(const FSonificationFeatures& Features,
	const FSonificationParams& Params)
{
	// Вымирание проверяется ПЕРВЫМ и раньше bValid: пустая сетка - это
	// измеренное состояние, а не отсутствие данных.
	if (Features.AliveCount <= 0)
	{
		return ESonificationShape::Extinct;
	}

	if (!Features.bValid)
	{
		return ESonificationShape::Idle;
	}

	if (Features.LogSlope <= -Params.CollapseSlope)
	{
		return ESonificationShape::Collapse;
	}

	// Колебание отличается от равновесия длиной ПУТИ, а не наклоном: и там и
	// там население никуда не пришло, но осциллятор всё это время двигался.
	if (Features.Oscillation01 >= Params.OscillationThreshold
		&& FMath::Abs(Features.LogSlope) < Params.SteadySlope)
	{
		return ESonificationShape::Oscillation;
	}

	if (Features.LogSlope <= -Params.SteadySlope)
	{
		return ESonificationShape::Decline;
	}

	if (Features.LogSlope >= Params.SteadySlope)
	{
		if (Features.Bend > Params.BendThreshold)
		{
			return ESonificationShape::Explosive;
		}

		// Здесь и живёт ловушка, ради которой классификация смотрит на две
		// величины сразу: в лог-координатах ЛИНЕЙНЫЙ рост неотличим от лёгкого
		// насыщения. У N = a*n наклон dy/dx = 1/n - положительный, убывающий, с
		// отрицательным изгибом, то есть ровно тот же признак, что у выходящей
		// на плато кривой. Различает их только показатель роста: линейный даёт
		// ровно 1.0, выдыхающийся - заметно меньше.
		if (Features.Bend < -Params.BendThreshold
			&& Features.GrowthExponent < Params.SaturationDimension)
		{
			return ESonificationShape::Saturation;
		}

		return ESonificationShape::Growth;
	}

	// Наклона нет, но кривая всё ещё гнётся вниз - рост только что кончился.
	if (Features.Bend < -Params.BendThreshold)
	{
		return ESonificationShape::Saturation;
	}

	return ESonificationShape::Steady;
}

FSonificationFeatures SonificationCurve::ComputeFeatures(
	const TArray<FGenerationSample>& History, const FSonificationParams& Params)
{
	FSonificationFeatures Features;

	if (History.Num() == 0)
	{
		Features.Shape = ESonificationShape::Idle;
		return Features;
	}

	const FGenerationSample& LastSample = History.Last();
	Features.Generation = LastSample.Generation;
	Features.AliveCount = LastSample.AliveCount;

	const double FullScale = FMath::Loge(1.0 + static_cast<double>(FMath::Max(Params.FullScalePopulation, 1)));
	if (FullScale > UE_DOUBLE_SMALL_NUMBER)
	{
		Features.Population01 = FMath::Clamp(
			static_cast<float>(LogPopulation(LastSample.AliveCount) / FullScale), 0.0f, 1.0f);
	}

	const int32 First = SonificationCurve::FindWindowStart(History, Params.WindowGenerations, Params.MinWindowSamples);
	const int32 Last = History.Num() - 1;
	Features.SampleCount = (First == INDEX_NONE) ? 0 : (Last - First + 1);
	Features.Confidence01 = FMath::Clamp(
		static_cast<float>(Features.SampleCount) / static_cast<float>(FMath::Max(Params.MinReliableSamples, 1)),
		0.0f, 1.0f);

	double Slope = 0.0;
	double Centroid = 0.0;
	Features.bValid = (First != INDEX_NONE)
		&& SonificationCurve::FitLogSlope(History, First, Last, Slope, Centroid);

	if (Features.bValid)
	{
		Features.LogSlope = static_cast<float>(Slope);

		double Curvature = 0.0;
		double Bend = 0.0;
		if (SonificationCurve::FitBend(History, First, Last, Curvature, Bend))
		{
			Features.LogCurvature = static_cast<float>(Curvature);
			Features.Bend = static_cast<float>(Bend);
		}

		// Путь и перемещение делятся на ОДИН И ТОТ ЖЕ интервал в поколениях,
		// поэтому в их отношении он сокращается, и дыры в замерах на
		// Oscillation01 не влияют. На саму Activity влияют - см. оговорку в
		// doc-comment поля.
		const int64 SpanGenerations = History[Last].Generation - History[First].Generation;
		if (SpanGenerations > 0)
		{
			double Path = 0.0;
			for (int32 Index = First; Index < Last; ++Index)
			{
				Path += FMath::Abs(LogPopulation(History[Index + 1].AliveCount)
					- LogPopulation(History[Index].AliveCount));
			}

			const double Displacement = FMath::Abs(LogPopulation(History[Last].AliveCount)
				- LogPopulation(History[First].AliveCount));

			Features.Activity = static_cast<float>(Path / static_cast<double>(SpanGenerations));
			Features.Oscillation01 = (Path > UE_DOUBLE_SMALL_NUMBER)
				? static_cast<float>(FMath::Clamp(1.0 - Displacement / Path, 0.0, 1.0))
				: 0.0f;
		}
	}

	// Размерность структуры - по ВСЕЙ истории, а не по окну: величина
	// медленная и глобальная, мерить её по шестидесяти четырём поколениям
	// значило бы ловить шум. Своей математики тут нет ни строчки, всё уже
	// посчитано для нормировки графика.
	Features.GrowthExponent = static_cast<float>(GenerationHistory::FitGrowthExponent(History));

	Features.Shape = SonificationCurve::ClassifyShape(Features, Params);
	return Features;
}

float SonificationCurve::SmoothTowards(float Current, float Target,
	float TimeConstantSeconds, float DeltaSeconds)
{
	if (TimeConstantSeconds <= 0.0f || DeltaSeconds <= 0.0f)
	{
		return (TimeConstantSeconds <= 0.0f) ? Target : Current;
	}

	const float Alpha = 1.0f - FMath::Exp(-DeltaSeconds / TimeConstantSeconds);
	return Current + (Target - Current) * Alpha;
}