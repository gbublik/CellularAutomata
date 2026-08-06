// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Automata/Sonification/AutomataSonifierComponent.h"
#include "Automata/Sonification/SonificationParameterNames.h"


UAutomataSonifierComponent* AAutomataOrchestrator::EnsureSonifier()
{
	if (IsValid(Sonifier))
	{
		return Sonifier;
	}

	// NewObject + RegisterComponent, а не CreateDefaultSubobject в конструкторе:
	// Live Coding не хот-патчит появление default-subobject'а на уже
	// расставленных в уровне акторах, и первая же горячая правка после такого
	// добавления уронила бы редактор. Идиом - EnsureHeadlight() на контроллере.
	Sonifier = NewObject<UAutomataSonifierComponent>(this, TEXT("AutomataSonifier"));
	if (Sonifier)
	{
		Sonifier->RegisterComponent();
	}

	return Sonifier;
}

void AAutomataOrchestrator::SetSonificationEnabled(bool bEnabled)
{
	bEnableSonification = bEnabled;

	if (UAutomataSonifierComponent* Component = EnsureSonifier())
	{
		Component->RefreshSettings();
	}

	// Тернарник внутри формат-строки Printf в UE 5.7 не компилируется вовсе -
	// строка проверяется consteval. Собираем if/else.
	FString Message;
	if (!bEnableSonification)
	{
		Message = TEXT("[P] Звук: выключен");
	}
	else
	{
		const FString PresetName = GetActiveSonificationPresetName();
		if (PresetName.IsEmpty())
		{
			Message = TEXT("[P] Звук: включён");
		}
		else
		{
			Message = FString::Printf(TEXT("[P] Звук: включён (%s)"), *PresetName);
		}
	}

	ShowStatusMessage(StatusKey_Sonification, Message);
}

TArray<FSonificationPreset> AAutomataOrchestrator::GetSonificationPresets() const
{
	return SonificationPresets::GetAll();
}

FString AAutomataOrchestrator::GetActiveSonificationPresetName() const
{
	const TArray<FSonificationPreset>& Presets = SonificationPresets::GetAll();
	return Presets.IsValidIndex(ActiveSonificationPresetIndex)
		? Presets[ActiveSonificationPresetIndex].Name
		: FString();
}

void AAutomataOrchestrator::ApplySonificationPreset(int32 PresetIndex)
{
	const TArray<FSonificationPreset>& Presets = SonificationPresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplySonificationPreset: индекс %d вне диапазона (наборов: %d)"),
			PresetIndex, Presets.Num());
		return;
	}

	const FSonificationPreset& Preset = Presets[PresetIndex];

	// Присваивание целиком, как у остальных таблиц пресетов: окно и постоянные
	// времени осмыслены только вместе, и остаток от предыдущего набора превратил
	// бы прибор в шум - измерение дрожит, а сглаживание об этом не знает.
	SonificationParams = Preset.Params;
	ActiveSonificationPresetIndex = PresetIndex;

	ShowStatusMessage(StatusKey_Sonification,
		FString::Printf(TEXT("[Shift+P] Звук: %s - %s"), *Preset.Name, *Preset.Description));

	UE_LOG(LogTemp, Log, TEXT("ApplySonificationPreset: '%s' (окно %d поколений, минимум %d замеров, сглаживание наклона %.2f с)"),
		*Preset.Name, SonificationParams.WindowGenerations, SonificationParams.MinWindowSamples,
		SonificationParams.TauSlope);
}

void AAutomataOrchestrator::CycleSonificationPreset()
{
	const TArray<FSonificationPreset>& Presets = SonificationPresets::GetAll();
	if (Presets.Num() == 0)
	{
		return;
	}

	const int32 NextIndex = Presets.IsValidIndex(ActiveSonificationPresetIndex)
		? (ActiveSonificationPresetIndex + 1) % Presets.Num()
		: 0;

	ApplySonificationPreset(NextIndex);
}

FString AAutomataOrchestrator::GetSonificationShapeName() const
{
	if (!IsValid(Sonifier))
	{
		return FString();
	}

	// Имена берутся из UMETA(DisplayName) самого перечисления, а не из
	// параллельной таблицы: две таблицы рано или поздно разъезжаются.
	const ESonificationShape Shape = Sonifier->GetLastFeatures().Shape;
	if (const UEnum* ShapeEnum = StaticEnum<ESonificationShape>())
	{
		return ShapeEnum->GetDisplayNameTextByValue(static_cast<int64>(Shape)).ToString();
	}

	return FString();
}

void AAutomataOrchestrator::LogSonificationContract() const
{
	// Инструкция по сборке графа печатается ИЗ ТЕХ ЖЕ констант, которые потом
	// рассылаются, поэтому разойтись с кодом она не может физически. Тем и
	// отличается от той же инструкции, записанной в документации.
	auto LogEntry = [](const FName& Name, const TCHAR* Type, const TCHAR* Meaning)
	{
		UE_LOG(LogTemp, Log, TEXT("  %-20s %-8s %s"), *Name.ToString(), Type, Meaning);
	};

	UE_LOG(LogTemp, Log, TEXT("=== Контракт сонификации: входы графа MetaSound ==="));
	UE_LOG(LogTemp, Log, TEXT("Фон (%s) - зацикленный MetaSoundSource, играет непрерывно:"),
		TEXT("SonificationBed"));

	LogEntry(SonificationParameters::Population, TEXT("float"), TEXT("0..1 население в логарифме"));
	LogEntry(SonificationParameters::Slope, TEXT("float"), TEXT("-1..1 знаковая скорость роста"));
	LogEntry(SonificationParameters::Growth, TEXT("float"), TEXT("0..1 только рост"));
	LogEntry(SonificationParameters::Decay, TEXT("float"), TEXT("0..1 только падение"));
	LogEntry(SonificationParameters::Curvature, TEXT("float"), TEXT("-1..1 изгиб: + разгон, - выдыхается"));
	LogEntry(SonificationParameters::Activity, TEXT("float"), TEXT("0..1 плотность шуршания"));
	LogEntry(SonificationParameters::Oscillation, TEXT("float"), TEXT("0..1 ходит и не приходит"));
	LogEntry(SonificationParameters::Dimension, TEXT("float"), TEXT("0..1 размерность d/3: нить-оболочка-объём"));
	LogEntry(SonificationParameters::DimensionRaw, TEXT("float"), TEXT("тот же d без нормировки"));
	LogEntry(SonificationParameters::Rate, TEXT("float"), TEXT("0..1 темп, поколений в секунду"));
	LogEntry(SonificationParameters::Liveness, TEXT("float"), TEXT("0..1 симуляция работает (гейт от дрона на паузе)"));
	LogEntry(SonificationParameters::Confidence, TEXT("float"), TEXT("0..1 доверие к окну"));
	LogEntry(SonificationParameters::Shape, TEXT("float"), TEXT("номер ESonificationShape - НЕ вести им звук"));
	LogEntry(SonificationParameters::Running, TEXT("bool"), TEXT("прогон идёт"));
	LogEntry(SonificationParameters::Extinct, TEXT("bool"), TEXT("сетка пуста"));
	LogEntry(SonificationParameters::OnExtinction, TEXT("trigger"), TEXT("сетка только что опустела"));
	LogEntry(SonificationParameters::OnReseed, TEXT("trigger"), TEXT("автоперекат сида"));
	LogEntry(SonificationParameters::OnStart, TEXT("trigger"), TEXT("прогон пошёл"));
	LogEntry(SonificationParameters::OnStop, TEXT("trigger"), TEXT("прогон остановлен"));
	LogEntry(SonificationParameters::OnReset, TEXT("trigger"), TEXT("счётчик поколений начат заново"));
	LogEntry(SonificationParameters::OnDispatch, TEXT("trigger"), TEXT("поколения состоялись (пачкой, не по одному)"));
	LogEntry(SonificationParameters::DispatchGenerations, TEXT("float"), TEXT("сколько поколений принёс заход"));

	UE_LOG(LogTemp, Log, TEXT("Клик по клетке (%s) - одиночный, позиционный:"), TEXT("CellClickSound"));
	LogEntry(SonificationParameters::Age01, TEXT("float"), TEXT("0..1 возраст / AgeColorMaxAge - совпадает с цветом"));
	LogEntry(SonificationParameters::AgeRaw, TEXT("float"), TEXT("возраст в поколениях"));
	LogEntry(SonificationParameters::Pitch01, TEXT("float"), TEXT("0..1 готовая высота"));
	LogEntry(SonificationParameters::Population, TEXT("float"), TEXT("фон на момент клика - чтобы нота была в ладу"));
	LogEntry(SonificationParameters::Dimension, TEXT("float"), TEXT("то же для тембра"));

	UE_LOG(LogTemp, Log, TEXT("Фон ОБЯЗАН быть зацикленным: движок молча глотает триггеры, пока компонент не играет."));
	UE_LOG(LogTemp, Log, TEXT("=== Конец контракта ==="));
}
