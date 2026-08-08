#include "Automata/Rendering/LightPresets.h"

const TArray<FLightPreset>& LightPresets::GetAll()
{
	// Собирается один раз при первом обращении - таблица константна, пересобирать
	// её на каждый вызов незачем (тот же приём, что в RenderPresets).
	static const TArray<FLightPreset> Presets = []()
	{
		TArray<FLightPreset> Result;

		{
			// Как в уровне: день, солнце с неба, студии нет. Первый в списке,
			// потому что это состояние по умолчанию - к нему возвращаются.
			FLightPreset& Preset = Result.AddDefaulted_GetRef();
			Preset.Name = TEXT("Солнце");
			Preset.bSunEnabled = true;
			Preset.SunIntensity = 10.0f;
			Preset.SunTemperature = 5500.0f;
			Preset.SunPitch = -45.0f;
			Preset.SunYaw = -30.0f;
			Preset.SkyIntensity = 1.0f;
			Preset.bStudioEnabled = false;
			Preset.KeyIntensity = 8.0f;
			Preset.KeyTemperature = 6000.0f;
			Preset.FillIntensity = 3.0f;
			Preset.FillTemperature = 4500.0f;
			Preset.RimIntensity = 6.0f;
			Preset.RimTemperature = 7500.0f;
		}

		{
			// Трёхточечная схема без солнца. Небо приглушено, но не погашено:
			// на нуле тени становятся угольными и вся теневая сторона структуры
			// пропадает из кадра.
			FLightPreset& Preset = Result.AddDefaulted_GetRef();
			Preset.Name = TEXT("Студия");
			Preset.bSunEnabled = false;
			Preset.SunIntensity = 0.0f;
			Preset.SunTemperature = 5500.0f;
			Preset.SunPitch = -45.0f;
			Preset.SunYaw = -30.0f;
			Preset.SkyIntensity = 0.3f;
			Preset.bStudioEnabled = true;
			Preset.KeyIntensity = 8.0f;
			Preset.KeyTemperature = 6000.0f;
			Preset.FillIntensity = 3.0f;
			Preset.FillTemperature = 4500.0f;
			Preset.RimIntensity = 6.0f;
			Preset.RimTemperature = 7500.0f;
		}

		{
			// Контровой правит бал: ключевой едва тлеет, силуэт обведён
			// холодным светом сзади. Для пористых структур - лучший способ
			// показать, что внутри есть полости: свет проходит насквозь.
			FLightPreset& Preset = Result.AddDefaulted_GetRef();
			Preset.Name = TEXT("Контровой");
			Preset.bSunEnabled = false;
			Preset.SunIntensity = 0.0f;
			Preset.SunTemperature = 5500.0f;
			Preset.SunPitch = -45.0f;
			Preset.SunYaw = -30.0f;
			Preset.SkyIntensity = 0.15f;
			Preset.bStudioEnabled = true;
			Preset.KeyIntensity = 2.0f;
			Preset.KeyTemperature = 5000.0f;
			Preset.FillIntensity = 0.5f;
			Preset.FillTemperature = 4000.0f;
			Preset.RimIntensity = 14.0f;
			Preset.RimTemperature = 9000.0f;
		}

		{
			// Ровный свет со всех сторон, теней почти нет. Не для красоты, а
			// для ЧТЕНИЯ: когда клетки покрашены по возрасту, тень врёт про
			// цвет, и разобрать, где какой слой, можно только на плоском свете.
			FLightPreset& Preset = Result.AddDefaulted_GetRef();
			Preset.Name = TEXT("Плоский");
			Preset.bSunEnabled = false;
			Preset.SunIntensity = 0.0f;
			Preset.SunTemperature = 5500.0f;
			Preset.SunPitch = -45.0f;
			Preset.SunYaw = -30.0f;
			Preset.SkyIntensity = 1.5f;
			Preset.bStudioEnabled = true;
			Preset.KeyIntensity = 4.0f;
			Preset.KeyTemperature = 6500.0f;
			Preset.FillIntensity = 4.0f;
			Preset.FillTemperature = 6500.0f;
			Preset.RimIntensity = 4.0f;
			Preset.RimTemperature = 6500.0f;
		}

		return Result;
	}();

	return Presets;
}
