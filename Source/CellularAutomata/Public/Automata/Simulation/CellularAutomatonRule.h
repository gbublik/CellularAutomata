#pragma once

#include "CoreMinimal.h"
#include "Automata/Simulation/Neighborhood.h"

/**
 * Правило клеточного автомата: клетка рождается/выживает по количеству
 * живых соседей. Три параметра, каждый независим и однозначен - никакого
 * строкового формата и парсинга ЗДЕСЬ: BirthCounts/SurvivalCounts - обычные
 * списки чисел (в отличие от классической Conway-нотации "B3/S23", здесь
 * не нужна отдельная запись для счётчиков >= 10, актуальных для Moore, до
 * 26 соседей). Чистый держатель параметров - сам расчёт шага делегирован
 * сменной FCellularAutomatonComputeStrategy (см. Automata/Simulation/
 * ComputeStrategy/), чтобы CPU/GPU/другие реализации могли переиспользовать
 * одни и те же параметры правила без дублирования.
 *
 * ПРИМЕЧАНИЕ про "никакого строкового формата" выше: это по-прежнему верно
 * для ЭТОГО класса - FCellularAutomatonRule как был, так и остаётся чистым
 * держателем уже разобранных чисел. Но см. Automata/Simulation/
 * RuleStringParser.h - отдельный, необязательный слой конвертации строки
 * вида "Survival/Birth/States/Neighborhood" (например "0-6/1,3/2/VN",
 * формат сайтов вроде williamyang98/3D-Cellular-Automata) в BirthCounts/
 * SurvivalCounts/Neighborhood. Это НЕ откат решения выше - тот аргумент был
 * конкретно про классическую однознаковую Conway-нотацию, ломающуюся на
 * двузначных счётчиках; формат RuleStringParser использует явные разделители
 * (","/"-") и однозначен при любом числе цифр, независимая нотация с
 * отдельным обоснованием.
 */
class CELLULARAUTOMATA_API FCellularAutomatonRule
{
public:
	/** InStates=2 (дефолт) - классический бинарный автомат, HasDecayStates()
	 *  возвращает false и ничего Generations-специфичного не задействуется
	 *  ни на одном из compute-путей (см. AAutomataOrchestrator::States).
	 *
	 *  InRadius=1 (дефолт) - соседство в одну клетку, как было до появления
	 *  радиуса: набор офсетов и их порядок совпадают с прежними побайтово для
	 *  обеих метрик (см. BuildNeighborOffsets()). */
	FCellularAutomatonRule(const TArray<int32>& BirthCounts, const TArray<int32>& SurvivalCounts, ENeighborhood InNeighborhood, int32 InStates = 2, int32 InRadius = 1);

	const TArray<FIntVector>& GetNeighborOffsets() const { return NeighborOffsets; }
	const TSet<int32>& GetBirthCounts() const { return BirthCounts; }
	const TSet<int32>& GetSurvivalCounts() const { return SurvivalCounts; }
	int32 GetStates() const { return States; }

	/** Радиус соседства в клетках - ровно то, что запросили (см.
	 *  AAutomataOrchestrator::NeighborhoodRadius). Осмыслен только для метрик;
	 *  у форм всегда 1, и для гало он НЕ годится - см. GetNeighborExtent(). */
	int32 GetNeighborRadius() const { return NeighborRadius; }

	/** Насколько далеко от клетки дотягивается самый дальний офсет (максимум
	 *  модуля компоненты по всему набору). Это и есть скорость роста
	 *  структуры: за поколение она расширяется максимум на столько клеток в
	 *  каждую сторону, поэтому GPU-пачке из K поколений нужно гало Extent*K
	 *  (см. FGpuComputeStrategy::StepBatch() - гало меньше нужного МОЛЧА
	 *  теряет пограничные клетки, без падения и без строчки в логе).
	 *
	 *  Считается по фактическим офсетам, а не берётся из радиуса, и это
	 *  принципиально: у форм с дальними осями радиус равен 1, а размах - 2.
	 *  Любая будущая форма получает верное гало бесплатно, просто потому что
	 *  оно выведено из геометрии, а не продублировано рядом с ней. */
	int32 GetNeighborExtent() const { return NeighborExtent; }

	/** Смещения соседей по метрике InNeighborhood в пределах радиуса Radius,
	 *  без центральной клетки. Публичная и статическая, потому что это чистая
	 *  геометрия, нужная и вне правила: StateGenerators::
	 *  AnalyzeNeighborCounts() считает по ней гистограмму соседей набора,
	 *  никаких BirthCounts/SurvivalCounts при этом не читая (раньше там лежал
	 *  свой дубликат этой таблицы).
	 *
	 *  При Radius == 1 обе ветки отдают ровно прежний набор в прежнем порядке -
	 *  специально, чтобы появление радиуса не могло ничего сдвинуть в уже
	 *  сохранённых прогонах. Порядок офсетов сам по себе ни на что не влияет
	 *  (и CPU-, и GPU-путь только суммируют по нему), но проверять это
	 *  рассуждением каждый раз дороже, чем сохранить его дословно. */
	static TArray<FIntVector> BuildNeighborOffsets(ENeighborhood InNeighborhood, int32 Radius = 1);

	/** true, если правило задействует Generations-угасание (States > 2) -
	 *  единственная точка входа, которую спрашивают CPU/GPU compute-стратегии
	 *  перед тем, как вообще посмотреть на IsDecaying()/угасающий буфер (см.
	 *  FCpuComputeStrategy::Step()/FGpuComputeStrategy::Step()) - при
	 *  States == 2 эта проверка - единственная лишняя работа на горячем
	 *  пути. */
	bool HasDecayStates() const { return States > 2; }

private:
	/** См. GetNeighborExtent(). Порядок полей ниже важен: NeighborExtent
	 *  инициализируется от уже построенного NeighborOffsets. */
	static int32 ComputeNeighborExtent(const TArray<FIntVector>& Offsets);

	TArray<FIntVector> NeighborOffsets;
	TSet<int32> BirthCounts;
	TSet<int32> SurvivalCounts;
	int32 States;
	int32 NeighborRadius;
	int32 NeighborExtent;
};
