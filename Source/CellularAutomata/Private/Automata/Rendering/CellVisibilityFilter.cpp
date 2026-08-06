#include "Automata/Rendering/CellVisibilityFilter.h"

#include "Automata/Grid/CellGrid.h"

namespace CellVisibility
{
	FFilter MakeSliceFilter(
		bool bSliceActive,
		const FVector& SliceOrigin,
		const FVector& SliceForward,
		float SliceDistance,
		float SliceThickness)
	{
		FFilter Filter;
		Filter.bSliceActive = bSliceActive;
		Filter.SliceOrigin = SliceOrigin;
		Filter.SliceForward = SliceForward;
		Filter.SliceMinDepth = SliceDistance - SliceThickness * 0.5;
		Filter.SliceMaxDepth = SliceDistance + SliceThickness * 0.5;
		return Filter;
	}

	void GatherAliveCells(const FCellGrid& Grid, const FBox* CullBounds, TArray<FIntVector>& OutCells)
	{
		if (CullBounds)
		{
			Grid.GetAliveCellsInBounds(*CullBounds, OutCells);
		}
		else
		{
			Grid.GetAliveCells(OutCells);
		}
	}
}
