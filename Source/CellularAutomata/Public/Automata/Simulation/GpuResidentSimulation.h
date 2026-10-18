#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FCellGrid;
class FCellularAutomatonRule;

/** Сводка, которую GPU-сессия отдаёт после каждого захода - всё, что
 *  возвращается на CPU, пока сессия идёт (32 байта вместо сетки). */
struct FGpuResidentStats
{
	int64 AliveCount = 0;
	/** Габарит живых клеток в координатах сетки. Имеет смысл при AliveCount > 0. */
	FIntVector MinAlive = FIntVector::ZeroValue;
	FIntVector MaxAlive = FIntVector::ZeroValue;
};

/**
 * Состояние автомата, которое живёт на GPU между заходами (docs/compute-
 * strategies.md, "Где уходит GpuRoundTrip": шаг на 90-95% тратился на то, чтобы
 * каждый раз заново собирать на CPU плотный объём из разреженной сетки).
 *
 * Сетка заливается один раз (Upload), дальше каждый Advance() крутит
 * поколения прямо в постоянных буферах видеокарты (тот же шейдер, что у
 * FGpuComputeStrategy, с возрастом и угасанием, которые всегда ведёт шейдер) и
 * читает назад только сводку. Download() - полное чтение назад, когда сетка
 * снова нужна на CPU (пауза, выделение, правка, сохранение).
 *
 * Объём - прямоугольник с запасом вокруг структуры. Перед каждым заходом
 * проверяется, что до края хватает места на N поколений роста (размах
 * окрестности * N); если нет - объём раздвигается: плоскости читаются назад и
 * перекладываются в новый, больший, прямо по плоскостям, без CPU-сетки. Это
 * редкое событие и единственный случай, когда заход стоит дорого.
 *
 * Все методы блокирующие и не потокобезопасные относительно друг друга:
 * владелец обязан не звать их одновременно (оркестратор держит не больше
 * одного захода в полёте). Буферы GPU трогает только render thread.
 */
class CELLULARAUTOMATA_API FGpuResidentSimulation
{
public:
	FGpuResidentSimulation();
	~FGpuResidentSimulation();

	FGpuResidentSimulation(const FGpuResidentSimulation&) = delete;
	FGpuResidentSimulation& operator=(const FGpuResidentSimulation&) = delete;

	/** Залить сетку в новый объём. MinHalo - сколько клеток запаса оставить
	 *  вокруг структуры как минимум (обычно размах * поколений за заход * 2).
	 *  false - объём не влез в MaxVolumeCells (OutError объясняет). */
	bool Upload(const FCellGrid& Grid, const FCellularAutomatonRule& Rule, int32 MinHalo, int64 MaxVolumeCells, FString& OutError);

	/** Посчитать NumSteps поколений. Возвращает, сколько посчитано (NumSteps),
	 *  либо 0 при отказе (OutError): объём не раздвинуть под лимит, или правило
	 *  сменило режим угасания - плоскость состояний есть только у сессии,
	 *  залитой под Generations. Отказ означает "закройте сессию". */
	int32 Advance(const FCellularAutomatonRule& Rule, int32 NumSteps, int64 MaxVolumeCells, FString& OutError);

	/** Прочитать состояние назад в пустую сетку: живые с возрастами и
	 *  угасающие со стадиями. */
	void Download(FCellGrid& OutGrid) const;

	bool IsUploaded() const { return VolumeCells > 0; }
	bool HasDecayPlane() const { return bDecayPlane; }
	const FGpuResidentStats& GetStats() const { return Stats; }
	FIntVector GetMinCell() const { return MinCell; }
	FIntVector GetVolumeDim() const { return VolumeDim; }
	int32 GetRegrowCount() const { return RegrowCount; }

private:
	struct FPlanes;
	struct FGpuState;

	/** Задать раскладку объёма по габариту и запасу, с ужатием запаса под лимит.
	 *  false - даже минимальный запас не влезает. */
	bool ChooseLayout(const FIntVector& BoundsMin, const FIntVector& BoundsMax, int32 MinHalo, int64 MaxVolumeCells,
		FIntVector& OutMinCell, FIntVector& OutDim, int64& OutCells, FString& OutError) const;

	/** Прочитать текущие плоскости с GPU (блокирующе). */
	void ReadPlanes(FPlanes& OutPlanes) const;

	/** Залить плоскости в новую раскладку (блокирующе). */
	void UploadPlanes(const FPlanes& Planes);

	/** Раздвинуть объём так, чтобы до края было не меньше RequiredBand клеток. */
	bool Regrow(int32 RequiredBand, int64 MaxVolumeCells, FString& OutError);

	TSharedRef<FGpuState, ESPMode::ThreadSafe> GpuState;

	FIntVector MinCell = FIntVector::ZeroValue;
	FIntVector VolumeDim = FIntVector::ZeroValue;
	int64 VolumeCells = 0;
	bool bDecayPlane = false;
	FGpuResidentStats Stats;
	int32 RegrowCount = 0;
};
