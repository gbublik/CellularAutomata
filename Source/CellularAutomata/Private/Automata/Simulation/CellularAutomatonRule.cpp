#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Automata/Grid/CellGrid.h"

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

TSet<int32> FCellularAutomatonRule::ParseCountSegment(const FString& Segment, const TCHAR* SegmentLabel)
{
	TSet<int32> Counts;

	if (Segment.Contains(TEXT(",")))
	{
		TArray<FString> Tokens;
		Segment.ParseIntoArray(Tokens, TEXT(","), /*bInCullEmpty=*/true);

		for (const FString& Token : Tokens)
		{
			const FString Trimmed = Token.TrimStartAndEnd();
			if (Trimmed.IsEmpty() || !Trimmed.IsNumeric())
			{
				UE_LOG(LogTemp, Warning, TEXT("FCellularAutomatonRule: не удалось разобрать сегмент %s - некорректный токен \"%s\""),
					SegmentLabel, *Token);
				return TSet<int32>();
			}
			Counts.Add(FCString::Atoi(*Trimmed));
		}
	}
	else
	{
		for (const TCHAR Ch : Segment)
		{
			if (!FChar::IsDigit(Ch))
			{
				UE_LOG(LogTemp, Warning, TEXT("FCellularAutomatonRule: не удалось разобрать сегмент %s - недопустимый символ '%c'"),
					SegmentLabel, Ch);
				return TSet<int32>();
			}
			Counts.Add(Ch - TEXT('0'));
		}
	}

	return Counts;
}

FCellularAutomatonRule::FCellularAutomatonRule(const FString& RuleString, ENeighborhood InNeighborhood)
{
	NeighborOffsets = BuildNeighborOffsets(InNeighborhood);

	FString BPart;
	FString SPart;
	if (!RuleString.Split(TEXT("/"), &BPart, &SPart))
	{
		UE_LOG(LogTemp, Warning, TEXT("FCellularAutomatonRule: не удалось разобрать правило \"%s\" - ожидается формат \"B.../S...\""), *RuleString);
		return;
	}

	if (!BPart.StartsWith(TEXT("B"), ESearchCase::CaseSensitive) ||
		!SPart.StartsWith(TEXT("S"), ESearchCase::CaseSensitive))
	{
		UE_LOG(LogTemp, Warning, TEXT("FCellularAutomatonRule: не удалось разобрать правило \"%s\" - ожидается формат \"B.../S...\""), *RuleString);
		return;
	}

	BirthCounts = ParseCountSegment(BPart.Mid(1), TEXT("B"));
	SurvivalCounts = ParseCountSegment(SPart.Mid(1), TEXT("S"));
}

void FCellularAutomatonRule::Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid) const
{
	TArray<FIntVector> AliveCells;
	CurrentGrid.GetAliveCells(AliveCells);

	TSet<FIntVector> Candidates;
	Candidates.Reserve(AliveCells.Num() * (NeighborOffsets.Num() + 1));
	for (const FIntVector& Cell : AliveCells)
	{
		Candidates.Add(Cell);
		for (const FIntVector& Offset : NeighborOffsets)
		{
			Candidates.Add(Cell + Offset);
		}
	}

	for (const FIntVector& Candidate : Candidates)
	{
		int32 AliveNeighborCount = 0;
		for (const FIntVector& Offset : NeighborOffsets)
		{
			if (CurrentGrid.IsAlive(Candidate + Offset))
			{
				++AliveNeighborCount;
			}
		}

		const bool bCurrentlyAlive = CurrentGrid.IsAlive(Candidate);
		const bool bNextAlive = bCurrentlyAlive
			? SurvivalCounts.Contains(AliveNeighborCount)
			: BirthCounts.Contains(AliveNeighborCount);

		if (bNextAlive)
		{
			NextGrid.SetAlive(Candidate, true);
		}
	}
}
