#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Grid/CellGrid.h"
#include "Async/ParallelFor.h"
#include "Algo/Sort.h"
#include "Algo/Unique.h"
#include "Containers/ArrayView.h"

TArray<FIntVector> FCellularAutomatonRule::BuildNeighborOffsets(ENeighborhood InNeighborhood)
{
	TArray<FIntVector> Offsets;

	if (InNeighborhood == ENeighborhood::VonNeumann)
	{
		Offsets.Reserve(6);
		Offsets.Add(FIntVector(1, 0, 0));
		Offsets.Add(FIntVector(-1, 0, 0));
		Offsets.Add(FIntVector(0, 1, 0));
		Offsets.Add(FIntVector(0, -1, 0));
		Offsets.Add(FIntVector(0, 0, 1));
		Offsets.Add(FIntVector(0, 0, -1));
		return Offsets;
	}

	// Moore: полный куб 3x3x3 без центра (26 соседей)
	Offsets.Reserve(26);
	for (int32 dx = -1; dx <= 1; ++dx)
	{
		for (int32 dy = -1; dy <= 1; ++dy)
		{
			for (int32 dz = -1; dz <= 1; ++dz)
			{
				if (dx == 0 && dy == 0 && dz == 0)
				{
					continue;
				}
				Offsets.Add(FIntVector(dx, dy, dz));
			}
		}
	}
	return Offsets;
}

FCellularAutomatonRule::FCellularAutomatonRule(const TArray<int32>& InBirthCounts, const TArray<int32>& InSurvivalCounts, ENeighborhood InNeighborhood)
	: NeighborOffsets(BuildNeighborOffsets(InNeighborhood))
	, BirthCounts(InBirthCounts)
	, SurvivalCounts(InSurvivalCounts)
{
}

void FCellularAutomatonRule::Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid) const
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
		[this, &AliveCells, SlotsPerCell](TArray<int32>& LocalHistogram, int32 AliveIndex)
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

	ParallelFor(AliveCells.Num(), [this, &AliveCells, &Scattered, &BucketCursors](int32 AliveIndex)
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

	ParallelFor(Candidates.Num(), [this, &Candidates, &CurrentGrid, &bNextAlive](int32 Index)
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
			? SurvivalCounts.Contains(AliveNeighborCount)
			: BirthCounts.Contains(AliveNeighborCount);
	});
	const double ParallelForSeconds = FPlatformTime::Seconds() - ParallelForStart;

	// Запись в NextGrid - последовательно: TSet/TBitArray внутри
	// FSparseCellGrid/FDenseCellGrid не потокобезопасны для конкурентной
	// записи, поэтому SetAlive() нельзя звать прямо из ParallelFor-лямбды.
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
