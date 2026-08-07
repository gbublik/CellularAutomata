#include "Orchestration/StabilityWindow.h"

void FStabilityWindow::Reset(int32 Capacity)
{
	Samples.Reset();
	Samples.SetNumZeroed(FMath::Max(Capacity, 2));
	NextIndex = 0;
	SampleCount = 0;
}

void FStabilityWindow::Clear()
{
	NextIndex = 0;
	SampleCount = 0;
}

void FStabilityWindow::Push(int32 AliveCount)
{
	if (Samples.Num() == 0)
	{
		// Ёмкость не задана - молча заводим окно по умолчанию, а не роняем
		// счёт: этот метод зовётся на каждом применённом поколении.
		Reset(16);
	}

	Samples[NextIndex] = AliveCount;
	NextIndex = (NextIndex + 1) % Samples.Num();
	// Не даём переполниться: дальше созревания счётчику расти незачем, а int32
	// на длинном прогоне переполнился бы.
	SampleCount = FMath::Min(SampleCount + 1, Samples.Num());
}

bool FStabilityWindow::IsStable(int32 Tolerance) const
{
	if (Samples.Num() == 0 || SampleCount < Samples.Num())
	{
		return false;
	}

	int32 Min = Samples[0];
	int32 Max = Samples[0];
	for (int32 Sample : Samples)
	{
		Min = FMath::Min(Min, Sample);
		Max = FMath::Max(Max, Sample);
	}

	return (Max - Min) <= FMath::Max(Tolerance, 0);
}
