#pragma once

#include "CoreMinimal.h"
#include "HexNeighborhood.generated.h"

/** Соседства на решётке из гексагональных призм (ELatticeType::HexPrism).
 *
 *  Отдельное перечисление, а НЕ новые значения ENeighborhood, и это важно:
 *  те четырнадцать - замкнутая таблица оболочек по d^2 над Z^3, а
 *  Rules.NeighborhoodShells проверяет именно её замкнутость (мощности
 *  6/12/8/6, классификацию по квадрату длины, алгебру объединений, инварианты
 *  чётности). Гекс-значения внутри неё сломали бы каждое из этих утверждений и
 *  потребовали бы переписать тест целиком. Раздельные перечисления не стоят
 *  ничего лишнего, потому что FCellularAutomatonRule умеет принимать список
 *  смещений напрямую.
 *
 *  Координаты аксиальные: (q, r) в плоскости, z - номер слоя. Шесть соседей в
 *  плоскости - это (+1,0), (-1,0), (0,+1), (0,-1), (+1,-1), (-1,+1); последняя
 *  пара и есть то, чем гекс отличается от квадрата - из восьми направлений
 *  квадратной сетки соседями объявлены шесть.
 *
 *  Все смещения лежат в {-1,0,1}, поэтому GetNeighborExtent() равен 1, и
 *  GPU-путь работает без единой правки: гало и упаковка рассчитаны на любой
 *  набор целых смещений, 20 штук умещаются в 26 слотов шейдера, а 20 соседей -
 *  в 32-битные маски Birth/Survival. */
UENUM(BlueprintType)
enum class EHexNeighborhood : uint8
{
	/** Только шесть в плоскости. Слои при этом ПОЛНОСТЬЮ независимы - это стопка
	 *  никак не связанных двумерных гексагональных автоматов, а не объёмный.
	 *  Вырожденный случай, но полезный: классические двумерные гекс-правила
	 *  проверяются именно на нём, и каждый слой можно считать отдельным
	 *  прогоном с тем же правилом. */
	Plane6,

	/** Шесть в плоскости плюс сосед сверху и снизу - 8. Прямой аналог
	 *  фон-Неймана: только смежные по грани призмы. */
	Prism8,

	/** Всё сразу: шесть в плоскости, два по вертикали и двенадцать диагональных
	 *  (каждый плоскостной сосед ещё и этажом выше/ниже) - 20. Аналог Moore для
	 *  призм. */
	Prism20
};

/** Читаемое имя - для логов и экранных сообщений, как
 *  GetNeighborhoodDisplayName() у кубических соседств. */
inline const TCHAR* GetHexNeighborhoodDisplayName(EHexNeighborhood Neighborhood)
{
	switch (Neighborhood)
	{
	case EHexNeighborhood::Plane6:  return TEXT("гекс: 6 в плоскости");
	case EHexNeighborhood::Prism8:  return TEXT("гекс-призма: 8");
	case EHexNeighborhood::Prism20: return TEXT("гекс-призма: 20");
	default:                        return TEXT("неизвестное");
	}
}

/** Смещения соседей. Единственное место, где задана гекс-геометрия, - добавить
 *  новое значение перечисления значит дописать сюда одну ветку.
 *
 *  Порядок внутри набора зафиксирован (сначала плоскость, потом вертикаль,
 *  потом диагонали) по той же причине, что и у кубического аналога: он попадает
 *  в фиксированный массив, уезжающий в шейдер, и от него зависит
 *  воспроизводимость сравнения CPU с GPU. */
inline TArray<FIntVector> BuildHexNeighborOffsets(EHexNeighborhood Neighborhood)
{
	// Шесть направлений гексагональной решётки в аксиальных координатах.
	static const FIntVector PlaneOffsets[6] = {
		FIntVector( 1,  0, 0), FIntVector(-1,  0, 0),
		FIntVector( 0,  1, 0), FIntVector( 0, -1, 0),
		FIntVector( 1, -1, 0), FIntVector(-1,  1, 0)
	};

	TArray<FIntVector> Offsets;
	Offsets.Reserve(20);

	for (const FIntVector& Offset : PlaneOffsets)
	{
		Offsets.Add(Offset);
	}

	if (Neighborhood == EHexNeighborhood::Prism8 || Neighborhood == EHexNeighborhood::Prism20)
	{
		Offsets.Add(FIntVector(0, 0,  1));
		Offsets.Add(FIntVector(0, 0, -1));
	}

	if (Neighborhood == EHexNeighborhood::Prism20)
	{
		for (const FIntVector& Offset : PlaneOffsets)
		{
			Offsets.Add(FIntVector(Offset.X, Offset.Y,  1));
			Offsets.Add(FIntVector(Offset.X, Offset.Y, -1));
		}
	}

	return Offsets;
}
