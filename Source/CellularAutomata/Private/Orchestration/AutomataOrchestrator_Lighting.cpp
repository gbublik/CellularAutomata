// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Core/PlayerController/GamePlayerController.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "EngineUtils.h"

// Свет. Солнце и небо ищутся в уровне и настраиваются отсюда; студийный риг
// живёт на контроллере (см. doc-comment блока в заголовке за тем, почему), и
// сюда попадает только пересылка настроек.
//
// Актёры ищутся КАЖДЫЙ раз, а не кэшируются: ровно тот же приём, что в
// ApplyBackgroundVisibility() рядом, и по той же причине - уровень
// редактируется, актёр может быть заменён или удалён, а живём мы в PIE, где
// невалидный кэш проявился бы падением, а не промахом.

namespace
{
	/** Первый ADirectionalLight в мире - он же солнце. Второго в этом уровне
	 *  нет и не предполагается: у направленного света в UE особая роль
	 *  (Atmosphere Sun Light), и два солнца дали бы два неба. */
	ADirectionalLight* FindSun(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<ADirectionalLight> It(World); It; ++It)
		{
			return *It;
		}
		return nullptr;
	}

	ASkyLight* FindSkyLight(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<ASkyLight> It(World); It; ++It)
		{
			return *It;
		}
		return nullptr;
	}
}

TArray<FLightPreset> AAutomataOrchestrator::GetLightPresets() const
{
	return LightPresets::GetAll();
}

void AAutomataOrchestrator::ApplyLightPreset(int32 PresetIndex)
{
	const TArray<FLightPreset>& Presets = LightPresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyLightPreset: индекс %d вне диапазона (пресетов: %d)"),
			PresetIndex, Presets.Num());
		return;
	}

	const FLightPreset& Preset = Presets[PresetIndex];

	// ВСЕ поля целиком, без исключений - см. doc-comment FLightPreset. Тумблер
	// "риг едет за камерой" сюда не входит и потому не трогается.
	bSunEnabled = Preset.bSunEnabled;
	SunIntensity = Preset.SunIntensity;
	SunTemperature = Preset.SunTemperature;
	SunPitch = Preset.SunPitch;
	SunYaw = Preset.SunYaw;
	SkyIntensity = Preset.SkyIntensity;

	bStudioLightsEnabled = Preset.bStudioEnabled;
	KeyLightIntensity = Preset.KeyIntensity;
	KeyLightTemperature = Preset.KeyTemperature;
	FillLightIntensity = Preset.FillIntensity;
	FillLightTemperature = Preset.FillTemperature;
	RimLightIntensity = Preset.RimIntensity;
	RimLightTemperature = Preset.RimTemperature;

	ActiveLightPresetIndex = PresetIndex;
	// Сбрасывается В КОНЦЕ, после всех присваиваний: сеттеры света поднимают
	// этот флаг сами, и сброс в начале был бы немедленно затёрт (тот же порядок,
	// что в ApplyRenderPreset()).
	bLightPresetModified = false;

	ApplyLightSettings();

	UE_LOG(LogTemp, Log, TEXT("ApplyLightPreset: '%s' (солнце %s, студия %s)"),
		*Preset.Name,
		Preset.bSunEnabled ? TEXT("вкл") : TEXT("выкл"),
		Preset.bStudioEnabled ? TEXT("вкл") : TEXT("выкл"));

	ShowStatusMessage(StatusKey_Lighting, FString::Printf(TEXT("Свет: %s"), *Preset.Name));
}

void AAutomataOrchestrator::ApplyLightSettings()
{
	UWorld* World = GetWorld();

	// --- Солнце ---
	if (ADirectionalLight* Sun = FindSun(World))
	{
		if (UDirectionalLightComponent* SunComponent = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			// Статичный свет в рантайме не меняется вовсе - ни яркость, ни
			// угол: его вклад запечён. Молчать об этом нельзя, иначе слайдер
			// в HUD будет ездить, а картинка стоять, и объяснить это нечем.
			// Предупреждаем один раз за сессию: настройка правится в уровне,
			// а не отсюда, и повторять это каждый кадр незачем.
			if (!Sun->GetRootComponent() || Sun->GetRootComponent()->Mobility != EComponentMobility::Movable)
			{
				if (!bSunMobilityWarned)
				{
					bSunMobilityWarned = true;
					UE_LOG(LogTemp, Warning, TEXT("ApplyLightSettings: у солнца в уровне Mobility не Movable - менять свет в рантайме нельзя. Поставьте Movable у ADirectionalLight в .umap"));
				}
			}
			else
			{
				Sun->SetActorRotation(FRotator(SunPitch, SunYaw, 0.0f));
			}

			SunComponent->SetVisibility(bSunEnabled);
			SunComponent->SetIntensity(SunIntensity);
			// Температура сама по себе не действует, пока не включён её учёт -
			// одна из тех настроек, которые молча ничего не делают.
			SunComponent->SetUseTemperature(true);
			SunComponent->SetTemperature(SunTemperature);
		}
	}

	// --- Небо ---
	if (ASkyLight* Sky = FindSkyLight(World))
	{
		if (USkyLightComponent* SkyComponent = Sky->GetLightComponent())
		{
			SkyComponent->SetIntensity(SkyIntensity);
		}
	}

	// --- Студийный риг ---
	// Компоненты на контроллере: его тик идёт всегда, а тик оркестратора - нет
	// (см. doc-comment блока света в заголовке).
	if (GamePC)
	{
		GamePC->RefreshStudioLights();
	}
}
