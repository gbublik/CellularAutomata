#include "Orchestration/GenerationHistory.h"

namespace
{
	/** Минимальная осмысленная ёмкость: по одной точке линию не построить. */
	constexpr int32 MinHistoryCapacity = 2;

	void TrimToCapacity(TArray<FGenerationSample>& History, int32 Capacity)
	{
		const int32 Cap = FMath::Max(MinHistoryCapacity, Capacity);
		if (History.Num() <= Cap)
		{
			return;
		}

		// Снимаем сразу всё лишнее, а не по одному элементу: GenerationHistoryCapacity
		// правится в Details panel на живом акторе, так что сюда можно прийти и
		// с только что уменьшенным значением, отстав от ёмкости на сотни замеров.
		//
		// EAllowShrinking::No - аллокация не освобождается и не отращивается
		// заново на следующем же замере: после прогрева буфер фиксирован, ровно
		// как и обещано "память не растёт".
		History.RemoveAt(0, History.Num() - Cap, EAllowShrinking::No);
	}

	/** Годится ли замер для нормировки: при Exponent > 0 поколение 0 дало бы
	 *  деление на ноль, и такой замер выбрасывается целиком. */
	bool IsSampleUsable(int64 Generation, double Exponent)
	{
		return Exponent <= 0.0 || Generation > 0;
	}

	/** Наибольшее "красивое" (1/2/5 * 10^k) значение, не превосходящее Value -
	 *  шаг сетки. Пара к NiceCeiling(), которая округляет вверх. */
	double NiceStep(double Value)
	{
		if (Value <= 0.0)
		{
			return 1.0;
		}

		const double Power = FMath::Pow(10.0, FMath::FloorToDouble(FMath::LogX(10.0, Value)));
		const double Fraction = Value / Power;

		double Nice = 1.0;
		if (Fraction >= 5.0)
		{
			Nice = 5.0;
		}
		else if (Fraction >= 2.0)
		{
			Nice = 2.0;
		}

		return Nice * Power;
	}
}

double GenerationHistory::NormalizedValue(int64 Generation, int32 Count, double Exponent)
{
	if (Exponent <= 0.0)
	{
		return double(Count);
	}

	// Вызывающий обязан был отсеять такой замер (см. IsSampleUsable) - но если
	// не отсеял, отдаём ноль, а не бесконечность: одна inf в массиве точек
	// уводит в NaN всю геометрию ломаной, и график исчезает целиком.
	if (Generation <= 0)
	{
		return 0.0;
	}

	return double(Count) / FMath::Pow(double(Generation), Exponent);
}

double GenerationHistory::FitGrowthExponent(const TArray<FGenerationSample>& History)
{
	// МНК по (log n, log P). Суммы копятся в double: окно короткое, а
	// однопроходная формула через средние стоила бы второго прохода.
	double SumX = 0.0;
	double SumY = 0.0;
	double SumXX = 0.0;
	double SumXY = 0.0;
	int32 Count = 0;

	for (const FGenerationSample& Sample : History)
	{
		// Логарифм нуля - минус бесконечность; вымершая сетка это законное
		// состояние, а не ошибка, просто точка в подгонке не участвует.
		if (Sample.Generation <= 0 || Sample.AliveCount <= 0)
		{
			continue;
		}

		const double X = FMath::Loge(double(Sample.Generation));
		const double Y = FMath::Loge(double(Sample.AliveCount));

		SumX += X;
		SumY += Y;
		SumXX += X * X;
		SumXY += X * Y;
		++Count;
	}

	if (Count < 2)
	{
		return 0.0;
	}

	// Знаменатель обнуляется, когда все пригодные замеры сидят на одном
	// поколении, - окно из одной вертикали, наклона у неё нет.
	const double Denominator = double(Count) * SumXX - SumX * SumX;
	if (FMath::IsNearlyZero(Denominator, UE_DOUBLE_SMALL_NUMBER))
	{
		return 0.0;
	}

	return (double(Count) * SumXY - SumX * SumY) / Denominator;
}

void GenerationHistory::ComputeNiceRange(double MinValue, double MaxValue,
	double& OutMin, double& OutMax)
{
	if (MaxValue < MinValue)
	{
		Swap(MinValue, MaxValue);
	}

	double Span = MaxValue - MinValue;
	if (Span <= 0.0)
	{
		// Полоса выродилась в точку (окно из одного замера, ровное плато).
		// Раздвигаем от самого значения, а не на фиксированную величину: у
		// отношения 1e-3 и у отношения 1e3 "чуть-чуть" - разные числа.
		Span = FMath::Max(FMath::Abs(MaxValue) * 0.1, UE_DOUBLE_SMALL_NUMBER);
	}

	// Запас по 10% с каждой стороны: кривая, идущая ровно по краю области, на
	// половину толщины линии срезается краем виджета.
	const double Padding = Span * 0.1;
	const double Low = MinValue - Padding;
	const double High = MaxValue + Padding;

	// Примерно на четыре деления - дальше округление само решит, сколько их
	// выйдет на самом деле.
	const double Step = NiceStep((High - Low) * 0.25);

	OutMin = FMath::FloorToDouble(Low / Step) * Step;
	OutMax = FMath::CeilToDouble(High / Step) * Step;

	// Округление вниз и вверх могло сойтись в одну точку только на полностью
	// вырожденных данных, но делить на ноль в MapToPoints() всё равно нельзя.
	if (OutMax <= OutMin)
	{
		OutMax = OutMin + Step;
	}
}

void GenerationHistory::Append(TArray<FGenerationSample>& History,
	int64 Generation, int32 AliveCount, int32 Capacity)
{
	FGenerationSample Sample;
	Sample.Generation = Generation;
	Sample.AliveCount = AliveCount;
	// Перенос вперёд, а не 0: см. doc-comment. Значение может быть тут же
	// исправлено на фактическое - если это поколение дойдёт до рендера,
	// NoteRendered() правит этот же замер на месте, в том же кадре.
	Sample.RenderedCount = History.Num() > 0 ? History.Last().RenderedCount : 0;

	History.Add(Sample);
	TrimToCapacity(History, Capacity);
}

void GenerationHistory::NoteRendered(TArray<FGenerationSample>& History,
	int64 Generation, int32 AliveCount, int32 RenderedCount, int32 Capacity)
{
	if (History.Num() > 0 && History.Last().Generation == Generation)
	{
		FGenerationSample& Last = History.Last();
		Last.RenderedCount = RenderedCount;
		// AliveCount тоже обновляем: DeleteSelectedCells() убивает клетки прямо
		// на паузе, без смены поколения, и обе цифры должны поехать сразу.
		Last.AliveCount = AliveCount;
		return;
	}

	FGenerationSample Sample;
	Sample.Generation = Generation;
	Sample.AliveCount = AliveCount;
	Sample.RenderedCount = RenderedCount;

	History.Add(Sample);
	TrimToCapacity(History, Capacity);
}

bool GenerationHistory::ComputeBounds(const TArray<FGenerationSample>& History, double Exponent,
	int64& OutMinGeneration, int64& OutMaxGeneration, double& OutMinY, double& OutMaxY)
{
	if (History.Num() == 0)
	{
		return false;
	}

	OutMinGeneration = 0;
	OutMaxGeneration = 0;
	OutMinY = 0.0;
	OutMaxY = 0.0;

	bool bFound = false;

	for (const FGenerationSample& Sample : History)
	{
		if (!IsSampleUsable(Sample.Generation, Exponent))
		{
			continue;
		}

		const double Alive = NormalizedValue(Sample.Generation, Sample.AliveCount, Exponent);
		const double Rendered = NormalizedValue(Sample.Generation, Sample.RenderedCount, Exponent);

		if (!bFound)
		{
			// Края окна больше нельзя брать концами массива: при нормировке
			// первые замеры из него выпадают, и левый край сдвигается.
			OutMinGeneration = Sample.Generation;
			OutMaxGeneration = Sample.Generation;
			// Дно ищется наравне с потолком, но нужно только нормированному
			// графику - обычному его выставит в ноль вызывающий.
			OutMinY = FMath::Min(Alive, Rendered);
			OutMaxY = FMath::Max(Alive, Rendered);
			bFound = true;
			continue;
		}

		OutMinGeneration = FMath::Min(OutMinGeneration, Sample.Generation);
		OutMaxGeneration = FMath::Max(OutMaxGeneration, Sample.Generation);
		// Границы по ОБОИМ рядам: при States > 2 "видимо" законно выше "всего",
		// и масштаб по одному ряду срезал бы второй.
		OutMinY = FMath::Min3(OutMinY, Alive, Rendered);
		OutMaxY = FMath::Max3(OutMaxY, Alive, Rendered);
	}

	return bFound;
}

double GenerationHistory::NiceCeiling(double Value)
{
	if (Value <= 0.0)
	{
		return 1.0;
	}

	const double Power = FMath::Pow(10.0, FMath::FloorToDouble(FMath::LogX(10.0, Value)));
	const double Fraction = Value / Power;

	double Nice = 10.0;
	if (Fraction <= 1.0)
	{
		Nice = 1.0;
	}
	else if (Fraction <= 2.0)
	{
		Nice = 2.0;
	}
	else if (Fraction <= 5.0)
	{
		Nice = 5.0;
	}

	return Nice * Power;
}

void GenerationHistory::MapToPoints(const TArray<FGenerationSample>& History,
	const FVector2f& PlotSize, const FVector2f& PlotOrigin,
	int64 MinGeneration, int64 MaxGeneration, double MinY, double MaxY,
	bool bLogScale, double Exponent,
	TArray<FVector2f>& OutAlivePoints, TArray<FVector2f>& OutRenderedPoints)
{
	OutAlivePoints.Reset(History.Num());
	OutRenderedPoints.Reset(History.Num());

	// В логарифме дно всегда ноль - см. doc-comment.
	const double Floor = bLogScale ? 0.0 : MinY;
	const double ValueSpan = MaxY - Floor;

	if (History.Num() == 0 || ValueSpan <= 0.0)
	{
		return;
	}

	const double GenerationSpan = double(MaxGeneration - MinGeneration);
	// Log(1 + V), а не Log(V): ноль обрабатывается бесплатно, а именно ноль -
	// нормальное значение для "видимо" (Ghost Shape, пустая сетка).
	const double LogTop = FMath::Loge(1.0 + MaxY);

	auto NormalizeValue = [Floor, MaxY, ValueSpan, bLogScale, LogTop](double Value) -> double
	{
		const double Clamped = FMath::Clamp(Value, Floor, MaxY);
		if (!bLogScale)
		{
			return (Clamped - Floor) / ValueSpan;
		}
		return LogTop > 0.0 ? FMath::Loge(1.0 + Clamped) / LogTop : 0.0;
	};

	for (int32 Index = 0; Index < History.Num(); ++Index)
	{
		const FGenerationSample& Sample = History[Index];

		// Поколение 0 при нормировке неопределено - точки для него нет вовсе.
		// Ряды остаются согласованными по длине: пропуск выкидывает замер из
		// обоих сразу.
		if (!IsSampleUsable(Sample.Generation, Exponent))
		{
			continue;
		}

		// По ЗНАЧЕНИЮ поколения, а не по индексу - см. doc-comment
		// FGenerationSample. Равномерная раскладка по индексу остаётся
		// запасным вариантом ровно для вырожденного окна (все замеры на одном
		// поколении - например, сразу после сброса), где делить не на что.
		double NormalizedX = 0.0;
		if (GenerationSpan > 0.0)
		{
			NormalizedX = double(Sample.Generation - MinGeneration) / GenerationSpan;
		}
		else if (History.Num() > 1)
		{
			NormalizedX = double(Index) / double(History.Num() - 1);
		}

		const float X = PlotOrigin.X + float(NormalizedX) * PlotSize.X;

		// 1 - Normalized: у виджета Y растёт вниз, а у значения - вверх.
		OutAlivePoints.Add(FVector2f(X, PlotOrigin.Y + float(1.0
			- NormalizeValue(NormalizedValue(Sample.Generation, Sample.AliveCount, Exponent))) * PlotSize.Y));
		OutRenderedPoints.Add(FVector2f(X, PlotOrigin.Y + float(1.0
			- NormalizeValue(NormalizedValue(Sample.Generation, Sample.RenderedCount, Exponent))) * PlotSize.Y));
	}
}
