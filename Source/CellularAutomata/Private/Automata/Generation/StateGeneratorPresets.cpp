#include "Automata/Generation/StateGeneratorPresets.h"

namespace StateGeneratorPresets
{
	namespace
	{
		/** Общая часть: имя, семейство, описание и тип. Остальные поля
		 *  вызывающий доводит на месте - у каждого семейства значимы свои. */
		FStateGeneratorPreset MakePreset(const TCHAR* Name, const TCHAR* FamilyName, const TCHAR* Description,
										 EStateGeneratorType Type)
		{
			FStateGeneratorPreset Preset;
			Preset.Name = Name;
			Preset.FamilyName = FamilyName;
			Preset.Description = Description;
			Preset.Params.Type = Type;
			return Preset;
		}
	}

	const TArray<FStateGeneratorPreset>& GetAll()
	{
		static const TArray<FStateGeneratorPreset> Presets = []()
		{
			TArray<FStateGeneratorPreset> Result;

			// --- Решётка / кристалл ------------------------------------------
			{
				// Тот самый случай, ради которого всё затевалось: одна ось,
				// толщина 1 - каждая живая клетка видит ровно 8 соседей по
				// Moore, а пустые над и под плитой - 9.
				FStateGeneratorPreset P = MakePreset(
					TEXT("Плоскости (2D-решётка)"), TEXT("Решётка"),
					TEXT("Стопка плоскостей толщиной в клетку. Все живые клетки видят ровно 8 соседей по Moore, примыкающие пустые - 9: правило с 8 в Survival и без 9 в Birth держит структуру вечно, а выбитая клетка запускает цепную реакцию."),
					EStateGeneratorType::LatticePlanes);
				P.Params.Extent = FIntVector(40, 40, 40);
				P.Params.Period = FIntVector(8, 8, 8);
				P.Params.Thickness = 1;
				P.Params.bAxisX = true;
				P.Params.bAxisY = false;
				P.Params.bAxisZ = false;
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Плоскости частые"), TEXT("Решётка"),
					TEXT("То же, но плиты идут вчетверо чаще - структура плотнее, реакция расходится быстрее."),
					EStateGeneratorType::LatticePlanes);
				P.Params.Extent = FIntVector(40, 40, 40);
				P.Params.Period = FIntVector(4, 4, 4);
				P.Params.Thickness = 1;
				P.Params.bAxisX = true;
				P.Params.bAxisY = false;
				P.Params.bAxisZ = false;
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Блоки 2x2x2"), TEXT("Решётка"),
					TEXT("Кубики 2x2x2 в узлах решётки. Живая клетка видит 7 соседей, примыкающая пустая - не больше 4: широкий зазор между Survival и Birth, самый снисходительный к подбору правила вариант."),
					EStateGeneratorType::LatticeBlocks);
				P.Params.Extent = FIntVector(40, 40, 40);
				P.Params.Period = FIntVector(8, 8, 8);
				P.Params.BlockSize = 2;
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Каркас"), TEXT("Решётка"),
					TEXT("Балки вдоль всех трёх осей - клетка ребра принадлежит линиям сразу двух осей. Единственная решётка с НЕоднородным числом соседей: перед подбором правила стоит посмотреть гистограмму."),
					EStateGeneratorType::LatticeFrame);
				P.Params.Extent = FIntVector(36, 36, 36);
				P.Params.Period = FIntVector(12, 12, 12);
				P.Params.Thickness = 1;
				P.Params.bAxisX = true;
				P.Params.bAxisY = true;
				P.Params.bAxisZ = true;
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Шахматная упаковка"), TEXT("Решётка"),
					TEXT("Половина объёма в шахматном порядке. И живые, и примыкающие пустые видят по 12 соседей по Moore, так что правило приходится разводить только разницей множеств. Под окрестностью von Neumann вырождается: живых соседей ровно 0."),
					EStateGeneratorType::LatticeCheckerboard);
				P.Params.Extent = FIntVector(30, 30, 30);
				P.Params.BlockSize = 1;
				Result.Add(MoveTemp(P));
			}

			// --- Заполненные тела --------------------------------------------
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Сплошной куб"), TEXT("Тела"),
					TEXT("Простой источник фронта, растущего наружу или внутрь."),
					EStateGeneratorType::SolidBox);
				P.Params.Extent = FIntVector(20, 20, 20);
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Сплошной шар"), TEXT("Тела"),
					TEXT("То же, но без углов - фронт расходится равномерно во все стороны."),
					EStateGeneratorType::SolidSphere);
				P.Params.Radius = 25;
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Полый шар"), TEXT("Тела"),
					TEXT("Оболочка в две клетки: фронт идёт сразу внутрь и наружу, а полость даёт им встретиться."),
					EStateGeneratorType::SphereShell);
				P.Params.Radius = 30;
				P.Params.Thickness = 2;
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Полый куб"), TEXT("Тела"),
					TEXT("Коробка со стенкой в две клетки - плоские грани против кривизны шара."),
					EStateGeneratorType::BoxShell);
				P.Params.Extent = FIntVector(25, 25, 25);
				P.Params.Thickness = 2;
				Result.Add(MoveTemp(P));
			}

			// --- Шум и кластеры ----------------------------------------------
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Равномерный шум"), TEXT("Шум"),
					TEXT("Точная плотность заполнения по всему объёму - в отличие от случайного шара, где число клеток задаётся бросками с повторами."),
					EStateGeneratorType::NoiseUniform);
				P.Params.Extent = FIntVector(25, 25, 25);
				P.Params.Density = 0.35f;
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Perlin-облака"), TEXT("Шум"),
					TEXT("Связные пятна вместо россыпи одиночек. Масштаб намеренно не круглый: в целочисленных точках Perlin равен нулю, и при масштабе 1.0 или 0.5 поле вырождается."),
					EStateGeneratorType::NoisePerlin);
				P.Params.Extent = FIntVector(40, 40, 40);
				P.Params.NoiseScale = 0.08f;
				P.Params.NoiseThreshold = 0.1f;
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Кластеры-зёрна"), TEXT("Шум"),
					TEXT("Разбросанные шарообразные зёрна: каждое живёт своей жизнью, а потом фронты встречаются. Добавление зерна не сдвигает уже подобранные - у каждого свой подпоток случайных чисел."),
					EStateGeneratorType::NoiseClusters);
				P.Params.Extent = FIntVector(40, 40, 40);
				P.Params.ClusterCount = 40;
				P.Params.ClusterRadius = 6;
				P.Params.ClusterRadiusJitter = 0.4f;
				P.Params.Density = 0.5f;
				Result.Add(MoveTemp(P));
			}

			// --- Симметричные затравки ---------------------------------------
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Симметричная затравка"), TEXT("Затравки"),
					TEXT("Мелкое случайное ядро, развёрнутое полной симметрией куба (48 копий). Классический способ получить из этих правил правильный симметричный фрактал вместо бесформенной кляксы."),
					EStateGeneratorType::SymmetricSeed);
				P.Params.CoreExtent = FIntVector(4, 4, 4);
				P.Params.Density = 0.5f;
				P.Params.Symmetry = ESeedSymmetry::FullCubic;
				Result.Add(MoveTemp(P));
			}
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Затравка-мельница"), TEXT("Затравки"),
					TEXT("Ядро, повёрнутое четырежды вокруг оси Z - симметрия, которой зеркала не дают: структура растёт с закруткой."),
					EStateGeneratorType::SymmetricSeed);
				P.Params.CoreExtent = FIntVector(6, 6, 2);
				P.Params.Density = 0.45f;
				P.Params.Symmetry = ESeedSymmetry::RotateZ4;
				Result.Add(MoveTemp(P));
			}

			// --- Легаси -------------------------------------------------------
			{
				FStateGeneratorPreset P = MakePreset(
					TEXT("Случайный шар"), TEXT("Шум"),
					TEXT("Ровно то, что делают кнопка Generate Random и хоткеи N/R: Amount бросков в шар, дубли не отслеживаются."),
					EStateGeneratorType::RandomBall);
				P.Params.Radius = 10;
				P.Params.Amount = 1000;
				Result.Add(MoveTemp(P));
			}

			return Result;
		}();

		return Presets;
	}
}
