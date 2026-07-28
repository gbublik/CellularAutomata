#include "Automata/Simulation/RulePresets.h"

namespace RulePresets
{
	namespace
	{
		/** Строит пресет, пересчитывая "плотность заполнения" в число
		 *  стартовых клеток (AAutomataOrchestrator::Amount).
		 *
		 *  Источник таблицы - williamyang98/3D-Cellular-Automata,
		 *  src/app/entries/system_entries.js: там каждый пресет задан
		 *  правилом плюс "рандомайзером" вида (Density, Radius), где Density -
		 *  доля клеток шара, которую надо засеять. У нас же GenerateRandom()
		 *  принимает не долю, а абсолютное Amount, и сеет reject-sampling'ом:
		 *  Amount случайных точек в шаре, округлённых до клетки, БЕЗ проверки
		 *  на повтор - две точки часто падают в одну клетку. Поэтому Amount =
		 *  Density * Объём даёт заметно меньшую реальную заполненность, чем
		 *  задумано в источнике (особенно на больших Density).
		 *
		 *  Считаем поэтому по формуле "задачи о коллекционере": чтобы покрыть
		 *  долю p из N ячеек случайными бросками с повторами, нужно в среднем
		 *  N * ln(1/(1-p)) бросков. Density = 1.0 при этом даёт бесконечность,
		 *  поэтому p зажата сверху в 0.95 (полное заполнение шара случайными
		 *  бросками недостижимо в принципе - только перебором клеток, а это
		 *  уже другой алгоритм генерации, не тот, что в GenerateRandom()).
		 *
		 *  N берём как объём шара 4/3*pi*r^3 - для решётки это оценка числа
		 *  клеток в шаре радиуса r, точная в пределе и достаточная здесь:
		 *  Amount в любом случае лишь стартовая подсказка, дизайнер правит его
		 *  в панели. */
		FRulePreset MakePreset(const TCHAR* Name, const TCHAR* RuleString, int32 SpawnRadius, double Density)
		{
			const double CellsInSphere = 4.0 / 3.0 * PI * FMath::Pow(static_cast<double>(SpawnRadius), 3.0);
			const double CoverFraction = FMath::Min(Density, 0.95);
			const double Draws = CellsInSphere * FMath::Loge(1.0 / (1.0 - CoverFraction));

			FRulePreset Preset;
			Preset.Name = Name;
			Preset.RuleString = RuleString;
			Preset.SpawnRadius = SpawnRadius;
			Preset.Amount = FMath::Max(1, FMath::RoundToInt(Draws));
			return Preset;
		}
	}

	const TArray<FRulePreset>& GetAll()
	{
		// Порядок и имена - как в system_entries.js источника (см. выше), чтобы
		// пресет в HUD можно было один в один сверить с сайтом. "Builder 1" и
		// "Builder 2" там действительно заданы одинаковым правилом и одинаковым
		// рандомайзером - это не опечатка при переносе, дубликат есть в самом
		// источнике; оставлен как есть, чтобы нумерация пресетов совпадала.
		//
		// Радиус: у Clouds 1 и Slow Decay в источнике он задан не в клетках, а
		// долей от размера сетки (Randomiser_Radius_Relative), что у нас не с
		// чем сопоставить - GridSize в этом проекте на генерацию не влияет
		// вовсе (см. GenerateRandom(): сеет по SpawnRadius, GridSize только
		// пишется в сейв). Поэтому им проставлен конкретный радиус 30 - шар
		// примерно на 113k клеток, обычный для этого проекта масштаб старта.
		static const TArray<FRulePreset> Presets = {
			MakePreset(TEXT("Amoeba-1"),         TEXT("9-26/5-7,12-13,15/16/M"),   8,  0.3),
			MakePreset(TEXT("445"),              TEXT("4/4/5/M"),                  15, 0.1),
			MakePreset(TEXT("Builder 2"),        TEXT("6,9/4,6,8-9/10/M"),         7,  0.35),
			MakePreset(TEXT("Crystal Growth 1"), TEXT("0-6/1,3/2/VN"),             1,  1.0),
			MakePreset(TEXT("Crystal Growth 2"), TEXT("1-3/1-3/5/VN"),             1,  1.0),
			MakePreset(TEXT("Clouds 1"),         TEXT("13-26/13-14,17-19/2/M"),    30, 0.5),
			MakePreset(TEXT("Slow Decay"),       TEXT("8,11,13-26/13-26/5/M"),     30, 0.45),
			MakePreset(TEXT("Spiky Growth 1"),   TEXT("7-26/4,12-13,15/10/M"),     7,  0.32),
			MakePreset(TEXT("Ripple Cube"),      TEXT("8-26/4,12-13,15/10/M"),     10, 0.35),
			MakePreset(TEXT("Amoeba-2"),         TEXT("9-26/5-7,12-13,15/5/M"),    5,  0.3),
			MakePreset(TEXT("Builder 1"),        TEXT("6,9/4,6,8-9/10/M"),         7,  0.35),
			MakePreset(TEXT("Pyroclastic"),      TEXT("4-7/6-8/10/M"),             5,  0.2),
			MakePreset(TEXT("678 678"),          TEXT("6-8/6-8/3/M"),              5,  0.3),
		};

		return Presets;
	}
}