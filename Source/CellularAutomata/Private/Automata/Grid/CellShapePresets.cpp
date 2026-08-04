#include "Automata/Grid/CellShapePresets.h"

/** Пять выпуклых многогранников, замощающих пространство параллельными
 *  переносами - и их ровно пять (Е. С. Фёдоров, "Начала учения о фигурах",
 *  1885). Классификация ЗАКРЫТА: шестого не существует, поэтому эта таблица
 *  имеет окончательную длину, а не растёт по мере добавления идей.
 *
 *  Порядок - по числу граней, оно же число соседей: 6, 8, 12, 12, 14.
 *
 *  ТРИ ИЗ ПЯТИ - ОДНО СЕМЕЙСТВО. Усечённый октаэдр, ромбододекаэдр и
 *  удлинённый додекаэдр живут на одной и той же ОЦК-подрешётке
 *  (ParityFilter = SameParity) и различаются ТОЛЬКО растяжением по Z:
 *
 *    Z = 1        -> усечённый октаэдр (14 граней)
 *    Z = sqrt(2)  -> ромбододекаэдр (12 ромбов); ОЦК, растянутая на sqrt(2), И ЕСТЬ ГЦК
 *    Z > sqrt(2)  -> удлинённый додекаэдр (8 ромбов + 4 шестиугольника)
 *
 *  Причина перехода ровно на sqrt(2): грань к соседу (0,0,+-2) существует,
 *  пока середина отрезка до него (она отходит от нуля на величину растяжения)
 *  ближе к нулю, чем к ближайшему диагональному соседу (а до того всегда
 *  ровно sqrt(2)). За порогом эти две грани исчезают, 14 превращается в 12.
 *
 *  Ромбододекаэдр в таблице всё же записан через ГЦК-фильтр (Even) с
 *  растяжением 1, а не через SameParity с растяжением sqrt(2) - обе записи
 *  дают одну и ту же картинку, но первая работала в проекте до появления
 *  растяжения, у неё целочисленный шаг решётки и она не зависит от точности
 *  иррационального множителя. */

namespace
{
	const TArray<FCellShapePreset>& BuildPresets()
	{
		static const TArray<FCellShapePreset> Presets = []()
		{
			TArray<FCellShapePreset> Result;

			{
				FCellShapePreset& Preset = Result.AddDefaulted_GetRef();
				Preset.Name = TEXT("Куб (простая кубическая)");
				Preset.Description = TEXT(
					"6 квадратных граней. Заселён каждый узел решётки, соседи - шесть по осям. "
					"Единственная форма, на которой работают все инструменты без оговорок: "
					"запекание меша отсекает грани именно по шести осевым соседям.");
				Preset.FaceCount = 6;
				Preset.ParityFilter = ECellParityFilter::None;
				Preset.Neighborhood = ENeighborhood::VonNeumann;
				Preset.LatticeZScale = 1.0f;
				Preset.CellMeshScaleMultiplier = 1.0f;
				Preset.ExpectedMeshAabb = FVector(1.0, 1.0, 1.0);
			}

			{
				FCellShapePreset& Preset = Result.AddDefaulted_GetRef();
				Preset.Name = TEXT("Гексагональная призма");
				Preset.Description = TEXT(
					"8 граней: 6 боковых прямоугольных + 2 шестиугольные крышки. Требует "
					"скошенного отображения в мир (X = (q + r/2)*a, Y = r*(sqrt(3)/2)*a) - "
					"ЕЩЁ НЕ РЕАЛИЗОВАНО, это второй этап. Ценна слоистостью: рост в плоскости "
					"и по вертикали идёт с разной скоростью, чего не бывает на куб-решётке.");
				Preset.FaceCount = 8;
				Preset.ParityFilter = ECellParityFilter::None;
				Preset.Neighborhood = ENeighborhood::VonNeumann;
				Preset.LatticeZScale = 1.0f;
				Preset.CellMeshScaleMultiplier = 1.0f;
				Preset.ExpectedMeshAabb = FVector(1.0, 1.154701, 1.0);
				Preset.bRequiresCustomMesh = true;
			}

			{
				FCellShapePreset& Preset = Result.AddDefaulted_GetRef();
				Preset.Name = TEXT("Ромбододекаэдр (ГЦК)");
				Preset.Description = TEXT(
					"12 ромбических граней. Заселены узлы с чётной суммой координат - это в "
					"точности гранецентрированная кубическая решётка, её 12 ближайших соседей "
					"стоят на одном расстоянии sqrt(2)*CellSize. Меш: 14 вершин / 24 ребра / "
					"12 граней, объём 2.0 при габарите 2x2x2.");
				Preset.FaceCount = 12;
				Preset.ParityFilter = ECellParityFilter::Even;
				Preset.Neighborhood = ENeighborhood::Edges;
				Preset.LatticeZScale = 1.0f;
				Preset.CellMeshScaleMultiplier = 2.0f;
				Preset.ExpectedMeshAabb = FVector(2.0, 2.0, 2.0);
			}

			{
				FCellShapePreset& Preset = Result.AddDefaulted_GetRef();
				Preset.Name = TEXT("Удлинённый додекаэдр (ОЦТ)");
				Preset.Description = TEXT(
					"12 граней: 8 ромбов + 4 шестиугольника. Та же ОЦК-подрешётка, что у "
					"усечённого октаэдра, но растянутая по Z вдвое - за порогом sqrt(2) две "
					"осевые грани исчезают, и 14 становится 12. Меш: 18 вершин / 28 рёбер / "
					"12 граней, объём РОВНО 8.0 при габарите 2x2x3; апексы смотрят по Z, "
					"шестиугольники - на +-X и +-Y.");
				Preset.FaceCount = 12;
				Preset.ParityFilter = ECellParityFilter::SameParity;
				// Честные 12 соседей - по одному на грань. Оболочками этот набор
				// не выражается (оболочка дальних осей содержит все шесть, а
				// нужны четыре), поэтому он задан списком - см.
				// ELatticeNeighborhood. Поле Neighborhood при этом остаётся
				// осмысленным запасным вариантом: CornersFarAxes даёт ту же
				// картинку, просто считает двух соседей, с которыми грани нет.
				Preset.Neighborhood = ENeighborhood::CornersFarAxes;
				Preset.NeighborhoodShape = ELatticeNeighborhood::ElongatedDodecahedron12;
				Preset.LatticeZScale = 2.0f;
				Preset.CellMeshScaleMultiplier = 2.0f;
				Preset.ExpectedMeshAabb = FVector(2.0, 2.0, 3.0);
				Preset.bRequiresCustomMesh = true;
			}

			{
				FCellShapePreset& Preset = Result.AddDefaulted_GetRef();
				Preset.Name = TEXT("Усечённый октаэдр (ОЦК)");
				Preset.Description = TEXT(
					"14 граней: 8 шестиугольных по диагоналям + 6 квадратных по осям. Заселены "
					"узлы, у которых все три координаты одной чётности - объёмноцентрированная "
					"кубическая решётка. Меш: 24 вершины / 36 рёбер / 14 граней, объём РОВНО "
					"4.0 при габарите 2x2x2.");
				Preset.FaceCount = 14;
				Preset.ParityFilter = ECellParityFilter::SameParity;
				Preset.Neighborhood = ENeighborhood::CornersFarAxes;
				Preset.LatticeZScale = 1.0f;
				Preset.CellMeshScaleMultiplier = 2.0f;
				Preset.ExpectedMeshAabb = FVector(2.0, 2.0, 2.0);
			}

			return Result;
		}();

		return Presets;
	}
}

const TArray<FCellShapePreset>& CellShapePresets::GetAll()
{
	return BuildPresets();
}
