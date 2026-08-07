#include "Automata/Editing/CellClipboard.h"

bool CellClipboard::ComputeBounds(const TArray<FIntVector>& Cells, FIntVector& OutMin, FIntVector& OutMax)
{
	if (Cells.Num() == 0)
	{
		return false;
	}

	OutMin = Cells[0];
	OutMax = Cells[0];
	for (const FIntVector& Cell : Cells)
	{
		OutMin.X = FMath::Min(OutMin.X, Cell.X);
		OutMin.Y = FMath::Min(OutMin.Y, Cell.Y);
		OutMin.Z = FMath::Min(OutMin.Z, Cell.Z);
		OutMax.X = FMath::Max(OutMax.X, Cell.X);
		OutMax.Y = FMath::Max(OutMax.Y, Cell.Y);
		OutMax.Z = FMath::Max(OutMax.Z, Cell.Z);
	}
	return true;
}

void CellClipboard::Normalize(TArray<FIntVector>& Cells)
{
	FIntVector Min, Max;
	if (!ComputeBounds(Cells, Min, Max))
	{
		return;
	}

	// Настоящее floor-деление, а не '/': координаты штатно отрицательные
	// (генерация центрирована в нуле), а усечение к нулю сдвинуло бы центр в
	// зависимости от знака - тот же капкан, что в чанковой арифметике
	// FDenseCellGrid и в переносе при сохранении.
	const FIntVector Center(
		FMath::DivideAndRoundDown(Min.X + Max.X, 2),
		FMath::DivideAndRoundDown(Min.Y + Max.Y, 2),
		FMath::DivideAndRoundDown(Min.Z + Max.Z, 2));

	for (FIntVector& Cell : Cells)
	{
		Cell -= Center;
	}
}

void CellClipboard::Rotate90(TArray<FIntVector>& Cells, int32 Axis, bool bClockwise)
{
	if (Cells.Num() == 0 || Axis < 0 || Axis > 2)
	{
		return;
	}

	for (FIntVector& Cell : Cells)
	{
		const FIntVector Old = Cell;
		switch (Axis)
		{
		case 0: // вокруг X: Y и Z меняются местами со сменой знака у одной из них
			Cell.Y = bClockwise ? -Old.Z : Old.Z;
			Cell.Z = bClockwise ? Old.Y : -Old.Y;
			break;
		case 1: // вокруг Y
			Cell.X = bClockwise ? Old.Z : -Old.Z;
			Cell.Z = bClockwise ? -Old.X : Old.X;
			break;
		default: // вокруг Z
			Cell.X = bClockwise ? -Old.Y : Old.Y;
			Cell.Y = bClockwise ? Old.X : -Old.X;
			break;
		}
	}

	// Вращение идёт вокруг нуля, а центр габарита с чётной стороной при этом
	// уезжает на полклетки - без пересчёта буфер отползал бы от курсора с
	// каждым поворотом, по чуть-чуть и незаметно.
	Normalize(Cells);
}

FIntVector CellClipboard::ComputePasteOrigin(const FIntVector& BufferMin, const FIntVector& BufferMax,
											 const FIntVector& BaseCell, const FIntVector& FaceNormal)
{
	FIntVector Origin = BaseCell;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (FaceNormal[Axis] > 0)
		{
			// Кладём на грань, смотрящую в плюс по этой оси: НИЖНИЙ край буфера
			// садится в первую свободную клетку снаружи.
			Origin[Axis] = BaseCell[Axis] - BufferMin[Axis];
		}
		else if (FaceNormal[Axis] < 0)
		{
			// Симметрично: подвешиваем под грань, верхний край буфера - в неё.
			Origin[Axis] = BaseCell[Axis] - BufferMax[Axis];
		}
		// Оси поперёк нормали (и весь вектор при нулевой нормали) буфер
		// центрирует на точке клика - он уже нормализован вокруг нуля, так что
		// это просто BaseCell.
	}

	return Origin;
}

void CellClipboard::Place(const TArray<FIntVector>& Buffer, const FIntVector& Origin, TArray<FIntVector>& OutCells)
{
	OutCells.Reset();
	OutCells.Reserve(Buffer.Num());
	for (const FIntVector& Cell : Buffer)
	{
		OutCells.Add(Cell + Origin);
	}
}
