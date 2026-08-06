#include "Automata/Sonification/AutomataSonifierComponent.h"

#include "AudioParameter.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"

#include "Automata/Sonification/SonificationCurve.h"
#include "Automata/Sonification/SonificationParameterNames.h"
#include "Orchestration/AutomataOrchestrator.h"

UAutomataSonifierComponent::UAutomataSonifierComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// После обновления симуляции: замер, добавленный из продолжения
	// AsyncTask(GameThread), читается в том же кадре, а не через один.
	// Задержка в кадр была бы неслышна, так что это гигиена, а не необходимость.
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UAutomataSonifierComponent::BeginPlay()
{
	Super::BeginPlay();

	LastGenerationChangeSeconds = FPlatformTime::Seconds();
	ResolveOrchestrator();
	EnsureBed();
}

void UAutomataSonifierComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (BedComponent)
	{
		BedComponent->Stop();
		BedComponent->DestroyComponent();
		BedComponent = nullptr;
	}

	for (TObjectPtr<UAudioComponent>& Voice : ClickVoices)
	{
		if (Voice)
		{
			Voice->Stop();
			Voice->DestroyComponent();
		}
	}
	ClickVoices.Reset();

	Super::EndPlay(EndPlayReason);
}

AAutomataOrchestrator* UAutomataSonifierComponent::ResolveOrchestrator()
{
	if (!IsValid(Orchestrator))
	{
		Orchestrator = Cast<AAutomataOrchestrator>(GetOwner());
	}
	return Orchestrator;
}

UAudioComponent* UAutomataSonifierComponent::CreateVoice(const TCHAR* Name, bool bSpatialized)
{
	AActor* Owner = GetOwner();
	if (!IsValid(Owner))
	{
		return nullptr;
	}

	UAudioComponent* Voice = NewObject<UAudioComponent>(Owner, Name);
	if (!Voice)
	{
		return nullptr;
	}

	Voice->bAutoActivate = false;
	Voice->bStopWhenOwnerDestroyed = true;
	Voice->bAllowSpatialization = bSpatialized;
	Voice->SetupAttachment(Owner->GetRootComponent());
	Voice->RegisterComponent();
	return Voice;
}

UAudioComponent* UAutomataSonifierComponent::EnsureBed()
{
	AAutomataOrchestrator* Owner = ResolveOrchestrator();
	if (!Owner)
	{
		return nullptr;
	}

	if (!Owner->SonificationBed)
	{
		if (!bMissingBedWarned)
		{
			bMissingBedWarned = true;
			UE_LOG(LogTemp, Warning,
				TEXT("Сонификация: фоновый звук не назначен (Automata|Audio -> SonificationBed). ")
				TEXT("Соберите MetaSource и укажите его в панели; список входов печатает кнопка ")
				TEXT("LogSonificationContract."));
		}
		return nullptr;
	}

	if (!IsValid(BedComponent))
	{
		BedComponent = CreateVoice(TEXT("SonificationBed"), /*bSpatialized=*/false);
		if (!BedComponent)
		{
			return nullptr;
		}
	}

	if (BedComponent->Sound != Owner->SonificationBed)
	{
		BedComponent->SetSound(Owner->SonificationBed);
		BedComponent->Stop();
	}

	BedComponent->SetVolumeMultiplier(FMath::Max(Owner->SonificationVolume, 0.0f));

	// Фон играет непрерывно и в тишине. Это требование, а не стиль:
	// SetTriggerParameter() у движка весь под if(IsPlaying()), и запуск по
	// событию терял бы вымирание и ресид беззвучно.
	if (!BedComponent->IsPlaying())
	{
		BedComponent->Play();
	}

	return BedComponent;
}

UAudioComponent* UAutomataSonifierComponent::AcquireClickVoice()
{
	AAutomataOrchestrator* Owner = ResolveOrchestrator();
	if (!Owner || !Owner->CellClickSound)
	{
		return nullptr;
	}

	const int32 VoiceCount = FMath::Clamp(Owner->ClickVoiceCount, 1, 32);
	if (ClickVoices.Num() != VoiceCount)
	{
		for (TObjectPtr<UAudioComponent>& Voice : ClickVoices)
		{
			if (Voice)
			{
				Voice->Stop();
				Voice->DestroyComponent();
			}
		}
		ClickVoices.Reset();

		for (int32 Index = 0; Index < VoiceCount; ++Index)
		{
			ClickVoices.Add(CreateVoice(*FString::Printf(TEXT("SonificationClick_%d"), Index),
				/*bSpatialized=*/true));
		}
		NextClickVoice = 0;
	}

	if (ClickVoices.Num() == 0)
	{
		return nullptr;
	}

	NextClickVoice = (NextClickVoice + 1) % ClickVoices.Num();
	UAudioComponent* Voice = ClickVoices[NextClickVoice];
	if (!IsValid(Voice))
	{
		return nullptr;
	}

	if (Voice->Sound != Owner->CellClickSound)
	{
		Voice->SetSound(Owner->CellClickSound);
	}

	// Аттенюация может лежать и на самом ассете - тогда поле оставляют пустым.
	// Если её нет ни там, ни там, звук будет плоским, и это надо знать заранее.
	Voice->bOverrideAttenuation = (Owner->ClickAttenuation != nullptr);
	Voice->AttenuationSettings = Owner->ClickAttenuation;
	Voice->SetVolumeMultiplier(FMath::Max(Owner->SonificationVolume, 0.0f));

	return Voice;
}

void UAutomataSonifierComponent::RefreshSettings()
{
	AAutomataOrchestrator* Owner = ResolveOrchestrator();
	if (!Owner)
	{
		return;
	}

	if (!Owner->bEnableSonification)
	{
		if (IsValid(BedComponent))
		{
			BedComponent->Stop();
		}
		return;
	}

	// Ассет могли назначить уже после первого предупреждения - тогда честно
	// дать подсистеме второй шанс и разрешить предупредить снова.
	bMissingBedWarned = false;
	bBedNotPlayingWarned = false;
	EnsureBed();
}

void UAutomataSonifierComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AAutomataOrchestrator* Owner = ResolveOrchestrator();
	if (!Owner)
	{
		return;
	}

	if (!Owner->bEnableSonification)
	{
		if (IsValid(BedComponent) && BedComponent->IsPlaying())
		{
			BedComponent->Stop();
		}
		return;
	}

	// Ранний выход ДО измерения: если играть нечем, математику считать незачем.
	if (!EnsureBed())
	{
		return;
	}

	const FHudStats& Stats = Owner->GetHudStats();
	const double Now = FPlatformTime::Seconds();

	LastFeatures = SonificationCurve::ComputeFeatures(Owner->GetGenerationSamples(),
		Owner->SonificationParams);

	DetectAndFireEvents(Stats.AliveCellCount, Stats.GenerationCount, Stats.AutoReseedCount,
		Stats.bSimulationRunning || Stats.bFastStepActive, Now);

	PushBedParameters(Stats, DeltaTime, Now);
}

void UAutomataSonifierComponent::DetectAndFireEvents(int32 AliveCount, int64 Generation,
	int32 AutoReseedCount, bool bRunning, double NowSeconds)
{
	// Первый тик только снимает базовую линию. Иначе он выстрелил бы разом
	// всеми триггерами - на старте всё "только что изменилось".
	if (!bHasEdgeBaseline)
	{
		bHasEdgeBaseline = true;
		LastAliveCount = AliveCount;
		LastGenerationCount = Generation;
		LastAutoReseedCount = AutoReseedCount;
		bLastRunning = bRunning;
		LastGenerationChangeSeconds = NowSeconds;
		return;
	}

	if (Generation > LastGenerationCount)
	{
		const int64 Advanced = Generation - LastGenerationCount;

		const double Interval = NowSeconds - LastGenerationChangeSeconds;
		if (Interval > 0.0 && Interval < 60.0)
		{
			MeasuredSampleInterval = (MeasuredSampleInterval > 0.0)
				? FMath::Lerp(MeasuredSampleInterval, Interval, 0.3)
				: Interval;
		}
		LastGenerationChangeSeconds = NowSeconds;

		if (IsValid(BedComponent))
		{
			BedComponent->SetFloatParameter(SonificationParameters::DispatchGenerations,
				static_cast<float>(Advanced));
		}
		FireTrigger(SonificationParameters::OnDispatch);
	}
	else if (Generation < LastGenerationCount)
	{
		// Счётчик пошёл назад - значит прогон начали заново. Одно условие
		// вместо разбора, кто из пяти путей позвал ResetGenerationCounter().
		// Базовую линию вымирания надо сбросить здесь же, иначе обнуление
		// сетки при сбросе прочлось бы как самостоятельное вымирание.
		LastGenerationChangeSeconds = NowSeconds;
		MeasuredSampleInterval = 0.0;
		FireTrigger(SonificationParameters::OnReset);
	}

	if (AliveCount <= 0 && LastAliveCount > 0)
	{
		FireTrigger(SonificationParameters::OnExtinction);
	}

	if (AutoReseedCount > LastAutoReseedCount)
	{
		FireTrigger(SonificationParameters::OnReseed);
	}

	if (bRunning != bLastRunning)
	{
		FireTrigger(bRunning ? SonificationParameters::OnStart : SonificationParameters::OnStop);
	}

	LastAliveCount = AliveCount;
	LastGenerationCount = Generation;
	LastAutoReseedCount = AutoReseedCount;
	bLastRunning = bRunning;
}

void UAutomataSonifierComponent::PushBedParameters(const FHudStats& Stats, float DeltaSeconds,
	double NowSeconds)
{
	AAutomataOrchestrator* Owner = ResolveOrchestrator();
	if (!Owner || !IsValid(BedComponent))
	{
		return;
	}

	const FSonificationParams& Tuning = Owner->SonificationParams;

	// Кадр может выйти длинным - AddInstances() на сотнях тысяч клеток стоит
	// десятки и сотни миллисекунд синхронно. Без подрезки один такой кадр
	// протащил бы все величины к цели рывком, то есть ровно тем щелчком, ради
	// устранения которого сглаживание и заведено.
	const float Dt = FMath::Min(DeltaSeconds, FMath::Max(Tuning.MaxSmoothStep, 0.01f));

	// На медленной скорости замеры приходят раз в несколько секунд, и короткая
	// постоянная времени превратила бы кривую в лестницу из ступенек.
	const float AdaptFloor = Tuning.bAdaptTauToStepRate
		? static_cast<float>(MeasuredSampleInterval * 0.5)
		: 0.0f;
	auto Tau = [AdaptFloor](float Configured)
	{
		return FMath::Max(Configured, AdaptFloor);
	};

	const FSonificationFeatures& F = LastFeatures;

	const float SlopeTarget = FMath::Clamp(F.LogSlope / FMath::Max(Tuning.SlopeFullScale, KINDA_SMALL_NUMBER), -1.0f, 1.0f);
	const float ActivityTarget = FMath::Clamp(F.Activity / FMath::Max(Tuning.ActivityFullScale, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
	const float DimensionTarget = FMath::Clamp(F.GrowthExponent / FMath::Max(Tuning.DimensionFullScale, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
	const float RateTarget = FMath::Clamp(Stats.GenerationsPerSecond / FMath::Max(Tuning.RateFullScale, KINDA_SMALL_NUMBER), 0.0f, 1.0f);

	// Гейт против дрона на паузе. НЕ падает, пока идёт фоновый счёт: диспатч на
	// семь секунд - это работа, а не простой, и приглушать фон ровно тогда,
	// когда машина считает тяжелее всего, было бы враньём.
	float LivenessTarget = 1.0f;
	const bool bBusy = Stats.bIsComputing || Stats.bSimulationRunning || Stats.bFastStepActive;
	if (!bBusy)
	{
		const double Grace = FMath::Max(static_cast<double>(Tuning.StaleGraceSeconds),
			2.0 * MeasuredSampleInterval);
		const double Fade = FMath::Max(static_cast<double>(Tuning.StaleFadeSeconds), 0.01);
		const double Stale = NowSeconds - LastGenerationChangeSeconds;
		LivenessTarget = static_cast<float>(FMath::Clamp(1.0 - (Stale - Grace) / Fade, 0.0, 1.0));
	}

	SmoothedPopulation = SonificationCurve::SmoothTowards(SmoothedPopulation, F.Population01, Tau(Tuning.TauPopulation), Dt);
	SmoothedSlope = SonificationCurve::SmoothTowards(SmoothedSlope, SlopeTarget, Tau(Tuning.TauSlope), Dt);
	SmoothedCurvature = SonificationCurve::SmoothTowards(SmoothedCurvature, F.Bend, Tau(Tuning.TauCurvature), Dt);
	SmoothedActivity = SonificationCurve::SmoothTowards(SmoothedActivity, ActivityTarget, Tau(Tuning.TauSlope), Dt);
	SmoothedOscillation = SonificationCurve::SmoothTowards(SmoothedOscillation, F.Oscillation01, Tau(Tuning.TauSlope), Dt);
	SmoothedDimension = SonificationCurve::SmoothTowards(SmoothedDimension, DimensionTarget, Tau(Tuning.TauDimension), Dt);
	SmoothedRate = SonificationCurve::SmoothTowards(SmoothedRate, RateTarget, Tau(Tuning.TauPopulation), Dt);
	SmoothedLiveness = SonificationCurve::SmoothTowards(SmoothedLiveness, LivenessTarget, Tau(Tuning.TauPopulation), Dt);

	// Одной командой на звуковой поток вместо пятнадцати.
	TArray<FAudioParameter> AudioParams;
	AudioParams.Reserve(16);
	AudioParams.Emplace(SonificationParameters::Population, SmoothedPopulation);
	AudioParams.Emplace(SonificationParameters::Slope, SmoothedSlope);
	AudioParams.Emplace(SonificationParameters::Growth, FMath::Max(SmoothedSlope, 0.0f));
	AudioParams.Emplace(SonificationParameters::Decay, FMath::Max(-SmoothedSlope, 0.0f));
	AudioParams.Emplace(SonificationParameters::Curvature, SmoothedCurvature);
	AudioParams.Emplace(SonificationParameters::Activity, SmoothedActivity);
	AudioParams.Emplace(SonificationParameters::Oscillation, SmoothedOscillation);
	AudioParams.Emplace(SonificationParameters::Dimension, SmoothedDimension);
	AudioParams.Emplace(SonificationParameters::DimensionRaw, F.GrowthExponent);
	AudioParams.Emplace(SonificationParameters::Rate, SmoothedRate);
	AudioParams.Emplace(SonificationParameters::Liveness, SmoothedLiveness);
	AudioParams.Emplace(SonificationParameters::Confidence, F.Confidence01);
	AudioParams.Emplace(SonificationParameters::Shape, static_cast<float>(static_cast<uint8>(F.Shape)));
	AudioParams.Emplace(SonificationParameters::Running, bBusy);
	AudioParams.Emplace(SonificationParameters::Extinct, Stats.AliveCellCount <= 0);

	BedComponent->SetParameters(MoveTemp(AudioParams));
}

void UAutomataSonifierComponent::FireTrigger(const FName& ParameterName)
{
	if (!IsValid(BedComponent))
	{
		return;
	}

	// Движок глотает триггер молча, если компонент не играет (весь
	// SetTriggerParameter() под if(IsPlaying())). Без этой проверки "событие не
	// слышно" было бы неотлаживаемо.
	if (!BedComponent->IsPlaying())
	{
		if (!bBedNotPlayingWarned)
		{
			bBedNotPlayingWarned = true;
			UE_LOG(LogTemp, Warning,
				TEXT("Сонификация: фон не играет, событие '%s' потеряно. ")
				TEXT("Проверьте, что SonificationBed - зацикленный MetaSource."),
				*ParameterName.ToString());
		}
		return;
	}

	BedComponent->SetTriggerParameter(ParameterName);
}

void UAutomataSonifierComponent::PlayCellClick(const FVector& WorldLocation, int32 Age, int32 MaxAge)
{
	AAutomataOrchestrator* Owner = ResolveOrchestrator();
	if (!Owner || !Owner->bEnableSonification)
	{
		return;
	}

	UAudioComponent* Voice = AcquireClickVoice();
	if (!Voice)
	{
		return;
	}

	const float Age01 = FMath::Clamp(static_cast<float>(Age) / static_cast<float>(FMath::Max(MaxAge, 1)), 0.0f, 1.0f);

	Voice->SetWorldLocation(WorldLocation);

	// Параметры ДО Play(). Неиграющему компоненту они не теряются - мержатся в
	// InstanceParameters и применяются при старте. Именно поэтому здесь пул
	// постоянных голосов, а не SpawnSoundAtLocation(): тот стартует немедленно,
	// и высота приехала бы с опозданием на звуковой блок.
	TArray<FAudioParameter> AudioParams;
	AudioParams.Reserve(5);
	AudioParams.Emplace(SonificationParameters::Age01, Age01);
	AudioParams.Emplace(SonificationParameters::AgeRaw, static_cast<float>(Age));
	AudioParams.Emplace(SonificationParameters::Pitch01, Age01);
	AudioParams.Emplace(SonificationParameters::Population, SmoothedPopulation);
	AudioParams.Emplace(SonificationParameters::Dimension, SmoothedDimension);
	Voice->SetParameters(MoveTemp(AudioParams));

	Voice->Play();
}