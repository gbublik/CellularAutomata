#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class FCellGrid;
class FCellularAutomatonRule;
class FRDGBuilder;

/** Сводка, которую GPU-сессия отдаёт после каждого захода - всё, что
 *  возвращается на CPU, пока сессия идёт (64 байта вместо сетки). */
struct FGpuResidentStats
{
	int64 AliveCount = 0;
	/** Габарит живых клеток в координатах сетки. Имеет смысл при AliveCount > 0. */
	FIntVector MinAlive = FIntVector::ZeroValue;
	FIntVector MaxAlive = FIntVector::ZeroValue;

	/** То же по угасающим (только правила Generations). Нужно раздвиганию:
	 *  угасающая клетка запрещает рождение в своей точке, терять её нельзя. */
	int64 DecayingCount = 0;
	FIntVector MinDecaying = FIntVector::ZeroValue;
	FIntVector MaxDecaying = FIntVector::ZeroValue;

	/** Что насчитал дополнительный проход владельца (FExtraPass), 0 без него. */
	int64 ExtraCount = 0;
};

/**
 * Плоскости сессии на видеокарте и их раскладка. Трогает ТОЛЬКО render
 * thread: сессия меняет их своими командами, а читатель (рисование клеток -
 * UCellGpuInstancesComponent) регистрирует буферы в своём графе тоже на
 * render thread. Поэтому раскладка продублирована здесь: копия в самой сессии
 * живёт на потоке, который зовёт Advance(), и может уйти вперёд.
 */
struct FGpuResidentPlanes
{
	/** Пинг-понг: Current - половина пары с последним посчитанным поколением. */
	TRefCountPtr<FRDGPooledBuffer> Bits[2];
	TRefCountPtr<FRDGPooledBuffer> Ages[2];
	/** Плоскость угасания; без правила Generations - заглушка в одно слово. */
	TRefCountPtr<FRDGPooledBuffer> Decay[2];
	int32 Current = 0;

	FIntVector MinCell = FIntVector::ZeroValue;
	FIntVector VolumeDim = FIntVector::ZeroValue;
	bool bDecayPlane = false;
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
 * окрестности * N); если нет - объём раздвигается: плоскости перекладываются
 * в новый, больший, прямо на видеокарте, по габариту из сводки.
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

	/** Дополнительный проход по итогу захода, в том же графе и до чтения
	 *  сводки. Может прибавить число в слово 15 сводки (FGpuResidentStats::
	 *  ExtraCount) - так рисование узнаёт число видимых клеток без отдельного
	 *  ожидания GPU. Зовётся на render thread; буферы - итоговое поколение. */
	using FExtraPass = TFunction<void(FRDGBuilder& GraphBuilder, const FGpuResidentPlanes& Planes,
		FRDGBuffer* Bits, FRDGBuffer* Ages, FRDGBuffer* Decay, FRDGBufferUAV* StatsUAV)>;

	/** Слово сводки, отданное FExtraPass. */
	static constexpr int32 ExtraStatsWord = 15;

	/** Байт видеопамяти на ячейку объёма: пары плоскостей бит, возрастов и
	 *  (у Generations) угасания. Для потолка объёма по памяти. */
	static double BytesPerCell(bool bWithDecay) { return 2.0 * (1.0 / 8.0 + 1.0 + (bWithDecay ? 1.0 : 0.0)); }

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
	int32 Advance(const FCellularAutomatonRule& Rule, int32 NumSteps, int64 MaxVolumeCells, FString& OutError,
		const FExtraPass& ExtraPass = FExtraPass());

	/** Прочитать состояние назад в пустую сетку: живые с возрастами и
	 *  угасающие со стадиями. */
	void Download(FCellGrid& OutGrid) const;

	bool IsUploaded() const { return VolumeCells > 0; }
	bool HasDecayPlane() const { return bDecayPlane; }
	const FGpuResidentStats& GetStats() const { return Stats; }
	FIntVector GetMinCell() const { return MinCell; }
	FIntVector GetVolumeDim() const { return VolumeDim; }
	int32 GetRegrowCount() const { return RegrowCount; }

	/** Буферы для читателя на render thread (см. FGpuResidentPlanes). */
	TSharedRef<FGpuResidentPlanes, ESPMode::ThreadSafe> GetGpuPlanes() const { return GpuState; }

private:
	struct FPlanes;

	/** Задать раскладку объёма по габариту и запасу, с ужатием запаса под лимит.
	 *  false - даже минимальный запас не влезает. */
	bool ChooseLayout(const FIntVector& BoundsMin, const FIntVector& BoundsMax, int32 MinHalo, int64 MaxVolumeCells,
		FIntVector& OutMinCell, FIntVector& OutDim, int64& OutCells, FString& OutError) const;

	/** Прочитать текущие плоскости с GPU (блокирующе). */
	void ReadPlanes(FPlanes& OutPlanes) const;

	/** Залить плоскости в новую раскладку (блокирующе). */
	void UploadPlanes(const FPlanes& Planes);

	/** Раздвинуть объём так, чтобы до края было не меньше RequiredBand клеток.
	 *  Прямо на видеокарте (CellularAutomatonRelayout.usf), по габариту из сводки. */
	bool Regrow(int32 RequiredBand, int64 MaxVolumeCells, FString& OutError);

	TSharedRef<FGpuResidentPlanes, ESPMode::ThreadSafe> GpuState;

	FIntVector MinCell = FIntVector::ZeroValue;
	FIntVector VolumeDim = FIntVector::ZeroValue;
	int64 VolumeCells = 0;
	bool bDecayPlane = false;
	FGpuResidentStats Stats;
	int32 RegrowCount = 0;
};
