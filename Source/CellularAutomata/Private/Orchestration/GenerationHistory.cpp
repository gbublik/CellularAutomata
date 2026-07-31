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

bool GenerationHistory::ComputeBounds(const TArray<FGenerationSample>& History,
	int64& OutMinGeneration, int64& OutMaxGeneration, int32& OutMaxY)
{
	if (History.Num() == 0)
	{
		return false;
	}

	// Замеры дописываются только в хвост и только с растущим номером поколения,
	// так что края окна - это его концы; сканировать нужно лишь максимум по Y.
	OutMinGeneration = History[0].Generation;
	OutMaxGeneration = History.Last().Generation;
	OutMaxY = 0;

	for (const FGenerationSample& Sample : History)
	{
		// Максимум по ОБОИМ рядам: при States > 2 "видимо" законно выше
		// "всего", и масштаб по одному ряду срезал бы второй.
		OutMaxY = FMath::Max3(OutMaxY, Sample.AliveCount, Sample.RenderedCount);
	}

	return true;
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
	int64 MinGeneration, int64 MaxGeneration, double MaxY, bool bLogScale,
	TArray<FVector2f>& OutAlivePoints, TArray<FVector2f>& OutRenderedPoints)
{
	OutAlivePoints.Reset(History.Num());
	OutRenderedPoints.Reset(History.Num());

	if (History.Num() == 0 || MaxY <= 0.0)
	{
		return;
	}

	const double GenerationSpan = double(MaxGeneration - MinGeneration);
	// Log(1 + V), а не Log(V): ноль обрабатывается бесплатно, а именно ноль -
	// нормальное значение для "видимо" (Ghost Shape, пустая сетка).
	const double LogTop = FMath::Loge(1.0 + MaxY);

	auto NormalizeValue = [MaxY, bLogScale, LogTop](int32 Value) -> double
	{
		const double Clamped = FMath::Clamp(double(Value), 0.0, MaxY);
		if (!bLogScale)
		{
			return Clamped / MaxY;
		}
		return LogTop > 0.0 ? FMath::Loge(1.0 + Clamped) / LogTop : 0.0;
	};

	for (int32 Index = 0; Index < History.Num(); ++Index)
	{
		const FGenerationSample& Sample = History[Index];

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
		OutAlivePoints.Add(FVector2f(X,
			PlotOrigin.Y + float(1.0 - NormalizeValue(Sample.AliveCount)) * PlotSize.Y));
		OutRenderedPoints.Add(FVector2f(X,
			PlotOrigin.Y + float(1.0 - NormalizeValue(Sample.RenderedCount)) * PlotSize.Y));
	}
}
