#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"
#include "Automata/Grid/CellGrid.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Async/ParallelFor.h"
#include "Algo/Sort.h"
#include "Algo/Unique.h"
#include "Containers/ArrayView.h"

void FCpuComputeStrategy::Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule) const
{
	const double GetAliveStart = FPlatformTime::Seconds();
	TArray<FIntVector> AliveCells;
	CurrentGrid.GetAliveCells(AliveCells);
	const double GetAliveSeconds = FPlatformTime::Seconds() - GetAliveStart;

	// Раньше дедуп кандидатов шёл через TSet<FIntVector>::Add (до 27 хэш-вставок
	// на живую клетку) - все на одном потоке. Промежуточный вариант (общая
	// сортировка плоского массива с дублями) тоже остался однопоточным и не
	// дал выигрыша: сортировка всего RAW-массива (с дублями) той же длины,
	// что и число хэш-вставок раньше, стоит примерно столько же.
	// Реальный фикс - разбить кандидатов по НЕЗАВИСИМЫМ корзинам (по хэшу
	// координаты, & BucketMask) и дедуплицировать каждую корзину на своём
	// потоке без какой-либо синхронизации между корзинами: любые дубликаты
	// одной и той же клетки гарантированно попадают в одну и ту же корзину
	// (хэш детерминирован), поэтому дедуп внутри корзины достаточен и корзины
	// независимы друг от друга.
	const double CandidateBuildStart = FPlatformTime::Seconds();
	const TArray<FIntVector>& NeighborOffsets = Rule.GetNeighborOffsets();
	const int32 SlotsPerCell = NeighborOffsets.Num() + 1;
	constexpr int32 NumBuckets = 256;
	constexpr uint32 BucketMask = NumBuckets - 1;

	// Шаг 1: подсчёт размера каждой корзины - параллельно по живым клеткам,
	// у каждой задачи свой локальный счётчик (ParallelForWithTaskContext),
	// без атомиков; локальные счётчики сливаются в общий один раз в конце
	// (сумма NumTasks*NumBuckets малых чисел - пренебрежимо дёшево).
	TArray<TArray<int32>> LocalHistograms;
	ParallelForWithTaskContext(LocalHistograms, AliveCells.Num(),
		[](int32, int32) { TArray<int32> Local; Local.SetNumZeroed(NumBuckets); return Local; },
		[&AliveCells, &NeighborOffsets, SlotsPerCell](TArray<int32>& LocalHistogram, int32 AliveIndex)
		{
			const FIntVector& Cell = AliveCells[AliveIndex];
			++LocalHistogram[GetTypeHash(Cell) & BucketMask];
			for (const FIntVector& Offset : NeighborOffsets)
			{
				++LocalHistogram[GetTypeHash(Cell + Offset) & BucketMask];
			}
		});

	TArray<int32> BucketCounts;
	BucketCounts.SetNumZeroed(NumBuckets);
	for (const TArray<int32>& Local : LocalHistograms)
	{
		for (int32 BucketIndex = 0; BucketIndex < NumBuckets; ++BucketIndex)
		{
			BucketCounts[BucketIndex] += Local[BucketIndex];
		}
	}

	TArray<int32> BucketOffsets;
	BucketOffsets.SetNumUninitialized(NumBuckets);
	int32 RawCandidateCount = 0;
	for (int32 BucketIndex = 0; BucketIndex < NumBuckets; ++BucketIndex)
	{
		BucketOffsets[BucketIndex] = RawCandidateCount;
		RawCandidateCount += BucketCounts[BucketIndex];
	}

	// Шаг 2: раскладка (scatter) всех кандидатов (с дублями) по корзинам -
	// снова параллельно по живым клеткам; место внутри корзины выдаёт
	// атомарный курсор (InterlockedIncrement), т.к. на одну и ту же корзину
	// пишут разные потоки конкурентно - но корзины между собой независимы.
	TArray<FIntVector> Scattered;
	Scattered.SetNumUninitialized(RawCandidateCount);
	TArray<int32> BucketCursors = BucketOffsets;

	ParallelFor(AliveCells.Num(), [&AliveCells, &NeighborOffsets, &Scattered, &BucketCursors](int32 AliveIndex)
	{
		const FIntVector& Cell = AliveCells[AliveIndex];
		auto ScatterOne = [&](const FIntVector& Candidate)
		{
			const uint32 Bucket = GetTypeHash(Candidate) & BucketMask;
			const int32 Slot = FPlatformAtomics::InterlockedIncrement(&BucketCursors[Bucket]) - 1;
			Scattered[Slot] = Candidate;
		};
		ScatterOne(Cell);
		for (const FIntVector& Offset : NeighborOffsets)
		{
			ScatterOne(Cell + Offset);
		}
	});

	// Шаг 3: сортировка + удаление дублей внутри каждой корзины - корзины не
	// пересекаются по памяти, поэтому это по-настоящему параллельно, без
	// какой-либо синхронизации между потоками.
	TArray<int32> FinalBucketCounts;
	FinalBucketCounts.SetNumUninitialized(NumBuckets);

	ParallelFor(NumBuckets, [&Scattered, &BucketOffsets, &BucketCounts, &FinalBucketCounts](int32 BucketIndex)
	{
		TArrayView<FIntVector> BucketView(Scattered.GetData() + BucketOffsets[BucketIndex], BucketCounts[BucketIndex]);
		Algo::Sort(BucketView, [](const FIntVector& A, const FIntVector& B)
		{
			if (A.X != B.X) { return A.X < B.X; }
			if (A.Y != B.Y) { return A.Y < B.Y; }
			return A.Z < B.Z;
		});
		FinalBucketCounts[BucketIndex] = Algo::Unique(BucketView);
	});

	TArray<int32> FinalBucketOffsets;
	FinalBucketOffsets.SetNumUninitialized(NumBuckets);
	int32 FinalCandidateCount = 0;
	for (int32 BucketIndex = 0; BucketIndex < NumBuckets; ++BucketIndex)
	{
		FinalBucketOffsets[BucketIndex] = FinalCandidateCount;
		FinalCandidateCount += FinalBucketCounts[BucketIndex];
	}

	// Шаг 4: сборка результата - каждая корзина копирует свой (уже
	// дедуплицированный) кусок в непересекающийся диапазон общего массива,
	// снова без синхронизации между потоками.
	TArray<FIntVector> Candidates;
	Candidates.SetNumUninitialized(FinalCandidateCount);

	ParallelFor(NumBuckets, [&Candidates, &Scattered, &BucketOffsets, &FinalBucketOffsets, &FinalBucketCounts](int32 BucketIndex)
	{
		if (FinalBucketCounts[BucketIndex] > 0)
		{
			FMemory::Memcpy(
				Candidates.GetData() + FinalBucketOffsets[BucketIndex],
				Scattered.GetData() + BucketOffsets[BucketIndex],
				FinalBucketCounts[BucketIndex] * sizeof(FIntVector));
		}
	});
	const double CandidateBuildSeconds = FPlatformTime::Seconds() - CandidateBuildStart;

	// Подсчёт соседей на кандидата не зависит от других кандидатов (только
	// читает CurrentGrid, которую Step() не мутирует) - самая тяжёлая часть
	// шага, безопасно распараллелить по CPU-потокам. Каждый поток пишет в
	// свой индекс TArray<bool> (не бит-упакованный в Unreal, в отличие от
	// std::vector<bool> - гонок по соседним элементам нет).
	const double ParallelForStart = FPlatformTime::Seconds();
	TArray<bool> bNextAlive;
	bNextAlive.SetNumUninitialized(Candidates.Num());

	// При States == 2 (подавляющее большинство правил) bDecayActive false, и
	// IsDecaying() ниже НИ РАЗУ не вызывается (короткое замыкание &&) -
	// единственная лишняя работа на кандидата это один захваченный локальный
	// bool. При States > 2 угасающая (не живая, но birth-immune) клетка не
	// должна родиться заново, даже если у неё "случайно" оказалось нужное
	// число живых соседей - см. doc-comment FCellGrid::IsDecaying() и
	// CellDecay::AdvanceDecayStates() за тем, что делает клетку угасающей.
	const bool bDecayActive = Rule.HasDecayStates();

	ParallelFor(Candidates.Num(), [&Candidates, &CurrentGrid, &NeighborOffsets, &Rule, &bNextAlive, bDecayActive](int32 Index)
	{
		const FIntVector& Candidate = Candidates[Index];
		int32 AliveNeighborCount = 0;
		for (const FIntVector& Offset : NeighborOffsets)
		{
			if (CurrentGrid.IsAlive(Candidate + Offset))
			{
				++AliveNeighborCount;
			}
		}
		const bool bCurrentlyAlive = CurrentGrid.IsAlive(Candidate);
		bNextAlive[Index] = bCurrentlyAlive
			? Rule.GetSurvivalCounts().Contains(AliveNeighborCount)
			: ((bDecayActive && CurrentGrid.IsDecaying(Candidate)) ? false
				: Rule.GetBirthCounts().Contains(AliveNeighborCount));
	});
	const double ParallelForSeconds = FPlatformTime::Seconds() - ParallelForStart;

	// Запись в NextGrid - последовательно: TBitArray внутри FDenseCellGrid
	// не потокобезопасен для конкурентной записи, поэтому SetAlive() нельзя
	// звать прямо из ParallelFor-лямбды.
	const double WriteBackStart = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		if (bNextAlive[Index])
		{
			NextGrid.SetAlive(Candidates[Index], true);
		}
	}
	const double WriteBackSeconds = FPlatformTime::Seconds() - WriteBackStart;

	const double TotalSeconds = GetAliveSeconds + CandidateBuildSeconds + ParallelForSeconds + WriteBackSeconds;
	UE_LOG(LogTemp, Log, TEXT("Step: живых %d -> кандидатов %d (шаг: %.2f мс [GetAliveCells: %.2f, CandidateBuild: %.2f, ParallelFor: %.2f, WriteBack: %.2f])"),
		AliveCells.Num(), Candidates.Num(), TotalSeconds * 1000.0,
		GetAliveSeconds * 1000.0, CandidateBuildSeconds * 1000.0, ParallelForSeconds * 1000.0, WriteBackSeconds * 1000.0);
}

bool FCpuComputeStrategy::CanStep(int32 AliveCount, int32 NeighborOffsetCount, FString& OutReason)
{
	const int64 SlotsPerCell = int64(NeighborOffsetCount) + 1;
	const int64 RawCandidates = int64(AliveCount) * SlotsPerCell;

	// Потолок индексации: TArray хранит длину в int32.
	if (RawCandidates > int64(MAX_int32))
	{
		const int64 MaxAlive = int64(MAX_int32) / SlotsPerCell;
		OutReason = FString::Printf(
			TEXT("кандидатов %lld (живых %d x %lld слотов) - больше, чем вмещает TArray (%d). Предел при таком соседстве: %lld живых клеток"),
			RawCandidates, AliveCount, SlotsPerCell, MAX_int32, MaxAlive);
		return false;
	}

	// И память: два массива по 12 байт на кандидата (рассеивание плюс итоговый
	// список). Запас в четверть - на всё остальное, что живёт в процессе;
	// упереться в ноль означало бы не отказ, а своп и повисший редактор.
	const int64 RequiredBytes = RawCandidates * int64(sizeof(FIntVector)) * 2;
	const int64 AvailableBytes = int64(FPlatformMemory::GetStats().AvailablePhysical);
	if (AvailableBytes > 0 && RequiredBytes > (AvailableBytes * 3) / 4)
	{
		OutReason = FString::Printf(
			TEXT("нужно ~%.1f ГБ под кандидатов (живых %d), свободно ~%.1f ГБ"),
			double(RequiredBytes) / (1024.0 * 1024.0 * 1024.0), AliveCount,
			double(AvailableBytes) / (1024.0 * 1024.0 * 1024.0));
		return false;
	}

	OutReason.Reset();
	return true;
}

int32 FCpuComputeStrategy::StepBatch(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule, int32 NumSteps) const
{
	FString Reason;
	if (!CanStep(CurrentGrid.Num(), Rule.GetNeighborOffsets().Num(), Reason))
	{
		// 0, а не 1: NextGrid осталась пустой, и подставить её вместо текущей
		// значило бы стереть структуру вместо отказа от шага.
		UE_LOG(LogTemp, Error, TEXT("FCpuComputeStrategy: шаг невозможен - %s. Сетка оставлена как была."), *Reason);
		return 0;
	}

	Step(CurrentGrid, NextGrid, Rule);
	return 1;
}
