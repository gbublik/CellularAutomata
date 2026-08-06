#include "Automata/Generation/CellArrayModifier.h"

namespace
{
	/** Count с наложенным ClampMin=1: панель его соблюдает, но структура
	 *  доступна из Blueprint напрямую, а нулевой или отрицательный счётчик
	 *  означал бы "ноль копий", то есть молча стёртую сцену. */
	FIntVector ClampedCount(const FCellArrayParams& Params)
	{
		return FIntVector(
			FMath::Max(Params.Count.X, 1),
			FMath::Max(Params.Count.Y, 1),
			FMath::Max(Params.Count.Z, 1));
	}
}

FIntVector CellArrayModifier::ComputeSize(const TArray<FIntVector>& Cells)
{
	if (Cells.Num() == 0)
	{
		return FIntVector::ZeroValue;
	}

	FIntVector Min = Cells[0];
	FIntVector Max = Cells[0];
	for (const FIntVector& Cell : Cells)
	{
		Min.X = FMath::Min(Min.X, Cell.X);
		Min.Y = FMath::Min(Min.Y, Cell.Y);
		Min.Z = FMath::Min(Min.Z, Cell.Z);
		Max.X = FMath::Max(Max.X, Cell.X);
		Max.Y = FMath::Max(Max.Y, Cell.Y);
		Max.Z = FMath::Max(Max.Z, Cell.Z);
	}

	// +1: габарит - это число клеток вдоль оси, а не расстояние между крайними.
	// Без единицы копии "впритык" (RelativeOffset = 1) наезжали бы ровно на одну
	// клетку по каждой оси - самая незаметная из возможных ошибок здесь.
	return FIntVector(Max.X - Min.X + 1, Max.Y - Min.Y + 1, Max.Z - Min.Z + 1);
}

FIntVector CellArrayModifier::ComputeStep(const FIntVector& SourceSize, const FCellArrayParams& Params)
{
	// RoundToInt, а не усечение: доля габарита - величина непрерывная, и
	// усечение к нулю на отрицательных шагах вело бы себя иначе, чем на
	// положительных (тираж в минус оказался бы плотнее тиража в плюс).
	return FIntVector(
		FMath::RoundToInt(SourceSize.X * Params.RelativeOffset.X) + Params.ConstantOffset.X,
		FMath::RoundToInt(SourceSize.Y * Params.RelativeOffset.Y) + Params.ConstantOffset.Y,
		FMath::RoundToInt(SourceSize.Z * Params.RelativeOffset.Z) + Params.ConstantOffset.Z);
}

int64 CellArrayModifier::EstimateCellCount(int64 SourceCellCount, const FCellArrayParams& Params)
{
	const FIntVector Count = ClampedCount(Params);
	return SourceCellCount * Count.X * Count.Y * Count.Z;
}

void CellArrayModifier::Tile(const TArray<FIntVector>& Source, const FCellArrayParams& Params,
							 TArray<FIntVector>& OutCells)
{
	OutCells.Reset();
	if (Source.Num() == 0)
	{
		return;
	}

	const FIntVector Count = ClampedCount(Params);
	const FIntVector Step = ComputeStep(ComputeSize(Source), Params);

	// Общий сдвиг всего тиража назад на половину его длины - см.
	// FCellArrayParams::bCenterOnSource. Делится (Count-1), а не Count: копий
	// Count, а промежутков между ними на один меньше, и центрировать надо
	// именно ЗАНЯТУЮ ими длину. Целочисленное деление здесь безопасно -
	// (Count-1) неотрицательно по построению, - но при чётном Count половина
	// не целая, и тираж оказывается на полшага смещён в минус.
	FIntVector CenterShift = FIntVector::ZeroValue;
	if (Params.bCenterOnSource)
	{
		CenterShift = FIntVector(
			-(Count.X - 1) * Step.X / 2,
			-(Count.Y - 1) * Step.Y / 2,
			-(Count.Z - 1) * Step.Z / 2);
	}

	OutCells.Reserve(Source.Num() * Count.X * Count.Y * Count.Z);

	for (int32 IndexX = 0; IndexX < Count.X; ++IndexX)
	{
		for (int32 IndexY = 0; IndexY < Count.Y; ++IndexY)
		{
			for (int32 IndexZ = 0; IndexZ < Count.Z; ++IndexZ)
			{
				const FIntVector Offset(
					IndexX * Step.X + CenterShift.X,
					IndexY * Step.Y + CenterShift.Y,
					IndexZ * Step.Z + CenterShift.Z);

				for (const FIntVector& Cell : Source)
				{
					OutCells.Add(Cell + Offset);
				}
			}
		}
	}
}
