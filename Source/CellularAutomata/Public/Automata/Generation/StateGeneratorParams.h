#pragma once

#include "CoreMinimal.h"
#include "Automata/Generation/CellParityFilter.h"
#include "Automata/Generation/LifePattern.h"
#include "Automata/Generation/SeedSymmetry.h"
#include "Automata/Generation/StateGeneratorType.h"
#include "StateGeneratorParams.generated.h"

/** Все параметры всех генераторов начального состояния в одной структуре -
 *  AAutomataOrchestrator::GenerationParams.
 *
 *  ОДНА структура, а не четыре по семействам, по трём причинам, из которых
 *  первая решающая:
 *
 *  1. EditCondition внутри USTRUCT резолвится в области видимости ЭТОЙ ЖЕ
 *     структуры - поле не видит UPROPERTY актора. Значит Type обязан лежать
 *     внутри; а раз он внутри, отдельные структуры дали бы либо дубль Type в
 *     каждой, либо потерю EditCondition вовсе.
 *  2. Одно поле - одна строка в BuildSaveHeader()/ApplySaveHeader(), если
 *     параметры генератора когда-нибудь поедут в .casave (FJsonObjectConverter
 *     разбирает вложенные USTRUCT сам, а UENUM пишет строкой, как
 *     ENeighborhood, - переупорядочивание перечисления старых файлов не ломает).
 *  3. HUD читает и пишет одну структуру ("Set members in struct") вместо
 *     switch'а по четырём.
 *
 *  Поля названы по РОЛИ и переиспользуются между семействами (Extent - у всех,
 *  Thickness - у решёток и оболочек): иначе смена типа генератора теряла бы
 *  уже настроенный размер области. Исключение - Amount: у RandomBall это число
 *  БРОСКОВ с повторами, а не доля заполнения, и путать его с Density нельзя.
 *
 *  EditConditionHides обязателен, а не декоративен: полей полтора десятка,
 *  а релевантны одновременно три-пять - без скрытия панель превращается в
 *  стену серых полей. */
USTRUCT(BlueprintType)
struct FStateGeneratorParams
{
	GENERATED_BODY()

	/** Что строим. Перебирается по кругу хоткеем Shift+Y
	 *  (AAutomataOrchestrator::CycleStateGeneratorType()). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation")
	EStateGeneratorType Type = EStateGeneratorType::LatticePlanes;

	/** ПОЛУразмер области построения в клетках: область - это
	 *  [-Extent, +Extent] по каждой оси, то есть 2*Extent+1 клеток, длина
	 *  НЕЧЁТНАЯ и центральная клетка лежит ровно в нуле.
	 *
	 *  Именно полуразмер, а не размер, потому что и период решётки, и
	 *  зеркалирование обязаны быть точно симметричны относительно нуля -
	 *  иначе узор "поедет" в центре кадра. Своё поле, а не GridSize актора:
	 *  GridSize на генерацию не влияет вовсе (он только пишется в сейв), и
	 *  тихо менять смысл сериализованного поля хуже, чем завести новое.
	 *
	 *  Ограничение сверху здесь - только чтобы слайдер не заводил в абсурд
	 *  одним движением мыши; от настоящей перегрузки защищает оценка числа
	 *  клеток (AAutomataOrchestrator::MaxGeneratedCells). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "512"))
	FIntVector Extent = FIntVector(40, 40, 40);

	/** Шаг решётки по каждой оси - расстояние между соседними узлами (блоками,
	 *  плитами, балками). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "1", UIMin = "2", UIMax = "64",
					  EditCondition = "Type == EStateGeneratorType::LatticeBlocks || Type == EStateGeneratorType::LatticeFrame || Type == EStateGeneratorType::LatticePlanes",
					  EditConditionHides))
	FIntVector Period = FIntVector(8, 8, 8);

	/** Сторона куба в узле решётки (LatticeBlocks) либо сторона ячейки
	 *  шахматной упаковки (LatticeCheckerboard). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "32",
					  EditCondition = "Type == EStateGeneratorType::LatticeBlocks || Type == EStateGeneratorType::LatticeCheckerboard",
					  EditConditionHides))
	int32 BlockSize = 2;

	/** Толщина плиты, балки или стенки оболочки в клетках. Для LatticePlanes
	 *  значение 1 - тот самый случай, когда каждая живая клетка видит ровно 8
	 *  соседей по Moore. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "32",
					  EditCondition = "Type == EStateGeneratorType::LatticeFrame || Type == EStateGeneratorType::LatticePlanes || Type == EStateGeneratorType::SphereShell || Type == EStateGeneratorType::BoxShell || Type == EStateGeneratorType::LifePattern",
					  EditConditionHides))
	int32 Thickness = 1;

	/** Радиус шара в клетках. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "500",
					  EditCondition = "Type == EStateGeneratorType::RandomBall || Type == EStateGeneratorType::SolidSphere || Type == EStateGeneratorType::SphereShell",
					  EditConditionHides))
	int32 Radius = 30;

	/** Число БРОСКОВ (не клеток!) для RandomBall: точки бросаются в шар с
	 *  повторами, поэтому живых клеток получится меньше. Такова была
	 *  исходная семантика AAutomataOrchestrator::Amount, и она сохранена
	 *  дословно. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "1",
					  EditCondition = "Type == EStateGeneratorType::RandomBall",
					  EditConditionHides))
	int32 Amount = 1000;

	/** Строить ли плиты (LatticePlanes) и балки (LatticeFrame) вдоль оси X. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (EditCondition = "Type == EStateGeneratorType::LatticeFrame || Type == EStateGeneratorType::LatticePlanes",
					  EditConditionHides))
	bool bAxisX = true;

	/** То же для оси Y. Для "одной плоскости" в смысле 2D-решётки нужно
	 *  оставить включённой ровно одну ось. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (EditCondition = "Type == EStateGeneratorType::LatticeFrame || Type == EStateGeneratorType::LatticePlanes",
					  EditConditionHides))
	bool bAxisY = false;

	/** То же для оси Z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (EditCondition = "Type == EStateGeneratorType::LatticeFrame || Type == EStateGeneratorType::LatticePlanes",
					  EditConditionHides))
	bool bAxisZ = false;

	/** Доля живых клеток: точная вероятность на клетку, в отличие от Amount. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0",
					  EditCondition = "Type == EStateGeneratorType::NoiseUniform || Type == EStateGeneratorType::NoiseClusters || Type == EStateGeneratorType::SymmetricSeed",
					  EditConditionHides))
	float Density = 0.35f;

	/** Масштаб Perlin-шума: во сколько раз сжимается координата перед выборкой.
	 *  Значение по умолчанию НЕ круглое намеренно - FMath::PerlinNoise3D()
	 *  возвращает ровно 0 в целочисленных точках, поэтому при масштабе 1.0,
	 *  0.5 или 0.25 поле вырождается и результат выходит либо пустым, либо
	 *  сплошным. SetStateGeneratorParams() предупреждает об этом в лог. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "0.001", UIMin = "0.01", UIMax = "0.5",
					  EditCondition = "Type == EStateGeneratorType::NoisePerlin",
					  EditConditionHides))
	float NoiseScale = 0.08f;

	/** Порог: клетка жива, если значение шума выше него. Значения Perlin лежат
	 *  примерно в +-0.7, а не в +-1, поэтому 0.1 даёт около трети заполнения. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "-1.0", ClampMax = "1.0", UIMin = "-0.7", UIMax = "0.7",
					  EditCondition = "Type == EStateGeneratorType::NoisePerlin",
					  EditConditionHides))
	float NoiseThreshold = 0.1f;

	/** Сколько зёрен разбросать по области. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "500",
					  EditCondition = "Type == EStateGeneratorType::NoiseClusters",
					  EditConditionHides))
	int32 ClusterCount = 40;

	/** Средний радиус зерна в клетках. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "100",
					  EditCondition = "Type == EStateGeneratorType::NoiseClusters",
					  EditConditionHides))
	int32 ClusterRadius = 6;

	/** Разброс радиуса зерна: 0.4 значит от 60% до 140% от ClusterRadius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "0.0", ClampMax = "0.9", UIMin = "0.0", UIMax = "0.9",
					  EditCondition = "Type == EStateGeneratorType::NoiseClusters",
					  EditConditionHides))
	float ClusterRadiusJitter = 0.4f;

	/** Полуразмер ядра, которое размножается симметрией. Ядро строится в
	 *  положительном октанте [0, CoreExtent], включая нулевые плоскости. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (ClampMin = "1", UIMin = "1", UIMax = "64",
					  EditCondition = "Type == EStateGeneratorType::SymmetricSeed",
					  EditConditionHides))
	FIntVector CoreExtent = FIntVector(4, 4, 4);

	/** Какими преобразованиями размножать ядро - см. ESeedSymmetry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (EditCondition = "Type == EStateGeneratorType::SymmetricSeed",
					  EditConditionHides))
	ESeedSymmetry Symmetry = ESeedSymmetry::FullCubic;

	/** Какой двумерный паттерн жизни выдавливать - см. ELifePattern. Толщину
	 *  задаёт Thickness, и осмысленное её значение здесь ровно одно: 2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation",
			  meta = (EditCondition = "Type == EStateGeneratorType::LifePattern",
					  EditConditionHides))
	ELifePattern LifePattern = ELifePattern::GosperGliderGun;

	/** Отбросить при генерации клетки "не той" чётности суммы координат, получив
	 *  ГЦК-подрешётку вместо кубической - см. ECellParityFilter, там же о том,
	 *  почему это даёт настоящую плотную упаковку и с каким соседством её
	 *  запускать (Edges или EdgesFarAxes).
	 *
	 *  Применяется ко ВСЕМ типам генераторов и потому без EditCondition: это не
	 *  параметр формы, а решётка, на которой форма строится. Отбор идёт в
	 *  FCellEmitter::Emit(), то есть в единственной воронке всех генераторов -
	 *  отброшенные клетки в массив не попадают вовсе, и потолок MaxGeneratedCells
	 *  считает реальные клетки, а не выброшенные.
	 *
	 *  На оценку EstimateCellCount() сознательно НЕ влияет: та документирована
	 *  как строгая оценка СВЕРХУ, подмножество её не нарушает, а делить её на
	 *  два было бы неверно - генератор вправе выдать целиком чётный набор
	 *  (LatticeBlocks с чётным Period), и тогда половинная оценка перестала бы
	 *  быть верхней границей. Оценка просто становится вдвое запасливее. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation")
	ECellParityFilter ParityFilter = ECellParityFilter::None;

	/** Посчитать после генерации гистограмму числа живых соседей - отдельно по
	 *  живым клеткам и по примыкающим к ним пустым - и вывести её в лог.
	 *
	 *  Это по-прежнему чистая геометрия: правило не читается и шаги не
	 *  гоняются, считается свойство самого множества точек. Но именно эта
	 *  таблица превращает подбор правила под структуру из гадания в
	 *  арифметику: если все живые клетки видят ровно 8 соседей, а примыкающие
	 *  пустые - 9, то правило с 8 в Survival и без 9 в Birth держит структуру
	 *  вечно, и выбитая из неё одна клетка запускает цепную реакцию.
	 *
	 *  Считается не по всему набору, а по центральному подкубу (структуры
	 *  периодичны, ответ тот же) - TSet на миллионы клеток стоил бы сотни
	 *  мегабайт ради той же гистограммы. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Generation")
	bool bAnalyzeNeighborCounts = true;
};
