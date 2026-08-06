#include "Automata/Sonification/SonificationPresets.h"

namespace
{
	FSonificationPreset MakePreset(const TCHAR* Name, const TCHAR* Description,
		const FSonificationParams& Params)
	{
		FSonificationPreset Preset;
		Preset.Name = Name;
		Preset.Description = Description;
		Preset.Params = Params;
		return Preset;
	}
}

const TArray<FSonificationPreset>& SonificationPresets::GetAll()
{
	static const TArray<FSonificationPreset> Presets =
	{
		// Индекс 0 - дорога назад: дефолтная конструкция структуры. Так набор
		// умолчаний физически не может разойтись с тем, что стоит в полях
		// FSonificationParams (тот же приём, что у CapturePresets).
		MakePreset(TEXT("Нейтральный"),
			TEXT("Окно 64 поколения, умеренное сглаживание - на все случаи"),
			FSonificationParams()),

		MakePreset(TEXT("Микроскоп"),
			TEXT("Окно 16 поколений, почти без сглаживания - слышно каждый шаг"),
			[]
			{
				// Короткое окно и короткие постоянные времени идут ТОЛЬКО
				// вместе: измерение по шестнадцати поколениям дрожит, и
				// сглаживание в полсекунды просто скрыло бы это дрожание,
				// оставив запаздывание без пользы.
				FSonificationParams Params;
				Params.WindowGenerations = 16;
				Params.MinWindowSamples = 4;
				Params.MinReliableSamples = 8;
				Params.TauPopulation = 0.05f;
				Params.TauSlope = 0.10f;
				Params.TauCurvature = 0.25f;
				Params.TauDimension = 0.6f;
				Params.bAdaptTauToStepRate = false;
				Params.StaleGraceSeconds = 0.3f;
				Params.StaleFadeSeconds = 0.5f;
				return Params;
			}()),

		MakePreset(TEXT("Обзор"),
			TEXT("Окно 512 поколений, длинное сглаживание - только крупная форма"),
			[]
			{
				FSonificationParams Params;
				Params.WindowGenerations = 512;
				Params.MinWindowSamples = 32;
				Params.MinReliableSamples = 64;
				Params.TauPopulation = 0.8f;
				Params.TauSlope = 2.0f;
				Params.TauCurvature = 3.0f;
				Params.TauDimension = 5.0f;
				Params.StaleGraceSeconds = 3.0f;
				Params.StaleFadeSeconds = 4.0f;
				return Params;
			}()),

		MakePreset(TEXT("Вымирание"),
			TEXT("Чувствителен к спаду и обвалу - для брутфорса сидов под Shift+N"),
			[]
			{
				// Смысл набора: услышать, что сид обречён, раньше, чем это
				// станет видно. Пороги спада занижены, шкала наклона сжата -
				// значит слой падения открывается уже на слабом убывании.
				FSonificationParams Params;
				Params.WindowGenerations = 32;
				Params.MinWindowSamples = 6;
				Params.SlopeFullScale = 0.15f;
				Params.SteadySlope = 0.003f;
				Params.CollapseSlope = 0.12f;
				Params.BendThreshold = 0.10f;
				Params.TauSlope = 0.2f;
				Params.TauCurvature = 0.5f;
				Params.StaleGraceSeconds = 0.5f;
				Params.StaleFadeSeconds = 0.8f;
				return Params;
			}())
	};

	return Presets;
}