#include "Automata/Editing/CellEditJournal.h"

#include "Automata/Grid/CellGrid.h"

namespace
{
	/** Снять текущее состояние клетки целиком - общее начало обеих Make*(). */
	FCellEdit CaptureCell(const FCellGrid& Grid, const FIntVector& Cell)
	{
		FCellEdit Edit;
		Edit.Cell = Cell;
		Edit.bWasAlive = Grid.IsAlive(Cell);
		Edit.PrevAge = Grid.GetAge(Cell);
		Edit.PrevDecayState = Grid.GetDecayState(Cell);
		return Edit;
	}
}

FCellEditRecord CellEditJournal::MakeDeleteRecord(const FCellGrid& Grid,
	const TArray<FIntVector>& Cells, int64 Generation)
{
	FCellEditRecord Record;
	Record.Generation = Generation;
	Record.Edits.Reserve(Cells.Num());

	for (const FIntVector& Cell : Cells)
	{
		if (!Grid.IsAlive(Cell))
		{
			// Мёртвую клетку "удалять" нечего - см. doc-comment функции.
			continue;
		}

		FCellEdit Edit = CaptureCell(Grid, Cell);
		Edit.bNowAlive = false;
		Record.Edits.Add(Edit);
	}

	Record.Description = FString::Printf(TEXT("удаление %d клеток"), Record.Edits.Num());
	return Record;
}

FCellEditRecord CellEditJournal::MakeAddRecord(const FCellGrid& Grid,
	const TArray<FIntVector>& Cells, int64 Generation)
{
	FCellEditRecord Record;
	Record.Generation = Generation;
	Record.Edits.Reserve(Cells.Num());

	for (const FIntVector& Cell : Cells)
	{
		if (Grid.IsAlive(Cell))
		{
			continue;
		}

		FCellEdit Edit = CaptureCell(Grid, Cell);
		Edit.bNowAlive = true;
		Record.Edits.Add(Edit);
	}

	Record.Description = FString::Printf(TEXT("добавление %d клеток"), Record.Edits.Num());
	return Record;
}

FCellEditRecord CellEditJournal::MakeMoveRecord(const FCellGrid& Grid,
	const TArray<FIntVector>& Cells, const FIntVector& Offset, int64 Generation)
{
	FCellEditRecord Record;
	Record.Generation = Generation;

	if (Offset == FIntVector::ZeroValue || Cells.Num() == 0)
	{
		Record.Description = TEXT("перенос 0 клеток");
		return Record;
	}

	// Живые из исходного набора - выделение переживает шаги симуляции, и клетка
	// под ним могла умереть (та же фильтрация, что в StartFromSelection()).
	TArray<FIntVector> Source;
	Source.Reserve(Cells.Num());
	for (const FIntVector& Cell : Cells)
	{
		if (Grid.IsAlive(Cell))
		{
			Source.Add(Cell);
		}
	}

	// Новый набор целиком - по нему решается судьба каждой затронутой клетки.
	TSet<FIntVector> Destination;
	Destination.Reserve(Source.Num());
	for (const FIntVector& Cell : Source)
	{
		Destination.Add(Cell + Offset);
	}

	// Затронуто объединение старого и нового: старые гаснут, новые загораются, а
	// пересечение (сдвиг меньше габарита - обычное дело) обязано быть учтено
	// РОВНО ОДИН РАЗ, иначе одна и та же клетка попала бы в запись дважды с
	// противоположными исходами.
	TSet<FIntVector> Touched;
	Touched.Reserve(Source.Num() * 2);
	Touched.Append(Destination);
	for (const FIntVector& Cell : Source)
	{
		Touched.Add(Cell);
	}

	Record.Edits.Reserve(Touched.Num());
	for (const FIntVector& Cell : Touched)
	{
		FCellEdit Edit = CaptureCell(Grid, Cell);
		Edit.bNowAlive = Destination.Contains(Cell);

		// Клетка, которая была живой и остаётся живой (пересечение старого и
		// нового), в записи не нужна: ни отменять, ни накатывать нечего. Но
		// возраст ей всё равно обнулится при ApplyForward(), так что оставить её
		// значило бы записать несуществующее изменение.
		if (Edit.bWasAlive == Edit.bNowAlive)
		{
			continue;
		}

		Record.Edits.Add(Edit);
	}

	Record.Description = FString::Printf(TEXT("перенос %d клеток"), Source.Num());
	return Record;
}

void CellEditJournal::ApplyForward(FCellGrid& Grid, const FCellEditRecord& Record)
{
	for (const FCellEdit& Edit : Record.Edits)
	{
		if (Edit.bNowAlive)
		{
			// Возраст 0 - клетка поставлена рукой сейчас, а не прожила
			// поколения (см. doc-comment FCellEdit).
			Grid.SetAliveWithAge(Edit.Cell, 0);
			Grid.SetDecayState(Edit.Cell, 0);
		}
		else
		{
			Grid.SetAlive(Edit.Cell, false);
			// Удаление рукой - это именно удаление, а не смерть по правилу:
			// клетка не уходит в угасание, иначе Generations-правила
			// продолжали бы рисовать её ещё States-2 поколений после того,
			// как её стёрли.
			Grid.SetDecayState(Edit.Cell, 0);
		}
	}
}

void CellEditJournal::ApplyInverse(FCellGrid& Grid, const FCellEditRecord& Record)
{
	for (const FCellEdit& Edit : Record.Edits)
	{
		if (Edit.bWasAlive)
		{
			Grid.SetAliveWithAge(Edit.Cell, Edit.PrevAge);
		}
		else
		{
			Grid.SetAlive(Edit.Cell, false);
		}

		// Фаза угасания возвращается независимо от живости: она осмыслена как
		// раз для НЕ живых клеток (см. FCellGrid::GetDecayState()).
		Grid.SetDecayState(Edit.Cell, Edit.PrevDecayState);
	}
}

int64 CellEditJournal::TotalCells(const TArray<FCellEditRecord>& Journal)
{
	int64 Total = 0;
	for (const FCellEditRecord& Record : Journal)
	{
		Total += Record.Edits.Num();
	}
	return Total;
}

void CellEditJournal::TrimAfter(TArray<FCellEditRecord>& Journal, int64 Generation)
{
	Journal.RemoveAll([Generation](const FCellEditRecord& Record)
	{
		return Record.Generation > Generation;
	});
}