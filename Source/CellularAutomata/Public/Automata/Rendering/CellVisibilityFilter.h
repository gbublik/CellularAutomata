#pragma once

#include "CoreMinimal.h"

class FCellGrid;

/** Три независимых отсечения, решающих, доходит ли клетка до экрана ДО того,
 *  как под неё построен хоть один трансформ: куб отсечения (ARenderCullVolume),
 *  срез вдоль взгляда (bEnableViewSlice) и фильтр по возрасту
 *  (AgeFilterValues). Терминологию стоит держать раздельно: culling прячет
 *  уже построенные инстансы по дистанции, а эти три убирают клетки из выборки
 *  вовсе - см. docs/rendering.md.
 *
 *  Вынесено сюда потому, что фильтрация нужна в ДВУХ местах:
 *  BuildCellRenderData() строит по ней инстансы, а ComputeVisibleCellsBounds()
 *  кадрирует камеру ровно по тому, что видно. Раньше это были две копии
 *  одного кода, связанные комментарием "поменяете один из трёх фильтров -
 *  поменяйте оба места"; ровно такой инвариант и разъезжается первым.
 *  Вызвать одну из другой нельзя: BuildCellRenderData() попутно красит клетки
 *  и обновляет учёт перестройки среза, а кадрированию не нужно ни то, ни
 *  другое.
 *
 *  ПОЧЕМУ ПРЕДИКАТЫ В ЗАГОЛОВКЕ, А НЕ В .cpp. Они зовутся на КАЖДУЮ живую
 *  клетку, то есть миллионы раз за кадр. Экспортируемая (CELLULARAUTOMATA_API)
 *  функция через границу модуля не инлайнится, и вынос предиката в .cpp
 *  превратил бы одно скалярное произведение в вызов - это был бы рефакторинг,
 *  оплаченный горячим путём рендера. Поэтому в .cpp уходит только СБОРКА
 *  фильтра (разово на рендер), а сами проверки остаются inline-методами
 *  структуры. Тестируются они всё равно свободно: FFilter это POD, ему не
 *  нужны ни актор, ни тик, ни рендер. */
namespace CellVisibility
{
	/** Параметры среза и фильтра возраста, посчитанные ОДИН раз на рендер.
	 *  Куба отсечения здесь нет намеренно: он применяется не поклеточно, а
	 *  выборкой из сетки (GatherAliveCells()) - спрашивать про него в цикле
	 *  значило бы заново проверять то, что уже сделано запросом по границам. */
	struct FFilter
	{
		/** Камера известна и срез включён. Когда false, PassesSlice() всегда
		 *  пропускает - GetCameraView() пишет Origin/Forward только при успехе. */
		bool bSliceActive = false;
		FVector SliceOrigin = FVector::ZeroVector;
		FVector SliceForward = FVector::ForwardVector;
		double SliceMinDepth = 0.0;
		double SliceMaxDepth = 0.0;

		/** Маска выбранных возрастов, развёрнутая ДО цикла: внутри остаётся
		 *  одно чтение по индексу вместо перебора выбранных возрастов на
		 *  каждой из миллионов клеток. Пустой фильтр - это bAgeFilterActive
		 *  == false, а не нулевой возраст: возраст 0 сам по себе законный слой. */
		bool bAgeFilterActive = false;
		TArray<bool> AgeMask;

		/** Ставится первой в цикле: отсекает больше всего и стоит одного
		 *  чтения из таблицы. */
		FORCEINLINE bool PassesAge(uint8 Age) const
		{
			return !bAgeFilterActive || AgeMask[Age];
		}

		/** Плоскость среза перпендикулярна взгляду, поэтому проверка - одно
		 *  скалярное произведение: глубина вдоль взгляда против диапазона.
		 *  Границы включительные, как и было в обеих копиях. */
		FORCEINLINE bool PassesSlice(const FVector& World) const
		{
			if (!bSliceActive)
			{
				return true;
			}

			const double Depth = FVector::DotProduct(World - SliceOrigin, SliceForward);
			return Depth >= SliceMinDepth && Depth <= SliceMaxDepth;
		}
	};

	/** Собирает диапазон глубин среза из "расстояние +- половина толщины" -
	 *  единственное место, где эта арифметика записана. */
	CELLULARAUTOMATA_API FFilter MakeSliceFilter(
		bool bSliceActive,
		const FVector& SliceOrigin,
		const FVector& SliceForward,
		float SliceDistance,
		float SliceThickness);

	/** Первый из трёх фильтров: живые клетки сетки, при непустом CullBounds -
	 *  только попавшие в него. Куб отсекается запросом по границам, а не
	 *  проверкой в цикле, чтобы сетка не отдавала заведомо лишние чанки. */
	CELLULARAUTOMATA_API void GatherAliveCells(
		const FCellGrid& Grid,
		const FBox* CullBounds,
		TArray<FIntVector>& OutCells);
}
