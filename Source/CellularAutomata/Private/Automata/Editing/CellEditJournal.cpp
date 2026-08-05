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