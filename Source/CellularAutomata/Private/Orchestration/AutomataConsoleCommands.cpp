// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"

/** Консольные команды `CA.*` - те же четыре таблицы пресетов и ввод правила
 *  строкой, что и в HUD, только из консоли (тильда в PIE).
 *
 *  ПОЧЕМУ НЕ UFUNCTION(Exec). Exec-функции консоль видит только на объектах
 *  своей цепочки (PlayerController, Pawn, GameMode, HUD, GameInstance) и только
 *  под именами без точек - то есть пришлось бы вешать их на контроллер, который
 *  и так владеет сорока хоткеями, и звать `CARulePreset 5`. Команды через
 *  IConsoleManager живут вне актора, зовутся `CA.RulePreset 5`, автодополняются
 *  по префиксу `CA.` и печатают свою справку - для набора из пяти команд это
 *  решает.
 *
 *  РЕГИСТРАЦИЯ СТАТИЧЕСКАЯ, не в BeginPlay() оркестратора: команда должна
 *  существовать и тогда, когда актора в мире нет, - иначе на пустой сцене её
 *  просто "не существует", и отличить это от опечатки в имени невозможно.
 *  Оркестратор ищется в момент вызова; не нашёлся - честная строка в ответ.
 *
 *  ВЫЗОВ БЕЗ АРГУМЕНТОВ ПЕЧАТАЕТ ТАБЛИЦУ. Это не украшение: индексы пресетов
 *  нигде больше не видны, а команда, требующая индекс и не умеющая его
 *  показать, бесполезна ровно до первого похода в исходники.
 *
 *  Печать идёт в FOutputDevice команды, а НЕ в UE_LOG - и список, и отказы.
 *  Вывод UE_LOG попадает в Output Log редактора, то есть не туда, куда смотрит
 *  человек, только что набравший команду в консоли под тильдой: первый же
 *  вопрос "а где список?" был именно об этом. В лог написанное всё равно
 *  попадёт - консоль пишет туда сама, - но сначала оно окажется на экране. */
namespace AutomataConsole
{
	/** Мир, в котором идёт игра. GetWorldContexts(), а не GWorld: в редакторе с
	 *  запущенным PIE их два, и GWorld указывает на редакторский - команда
	 *  молча правила бы актор, которого никто не тикает (та же ловушка, что с
	 *  CallInEditor-кнопками при выделенном не-PIE акторе, см. docs/input-and-camera.md). */
	UWorld* FindGameWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
			{
				if (Context.World())
				{
					return Context.World();
				}
			}
		}
		return nullptr;
	}

	AAutomataOrchestrator* FindOrchestrator(FOutputDevice& Ar)
	{
		UWorld* World = FindGameWorld();
		if (!World)
		{
			Ar.Logf(ELogVerbosity::Warning, TEXT("CA.*: игровой мир не найден - команда работает в PIE или в игре"));
			return nullptr;
		}

		AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(
			UGameplayStatics::GetActorOfClass(World, AAutomataOrchestrator::StaticClass()));
		if (!Orchestrator)
		{
			Ar.Logf(ELogVerbosity::Warning, TEXT("CA.*: AAutomataOrchestrator не найден в мире"));
		}
		return Orchestrator;
	}

	/** Разбирает индекс из первого аргумента. false - аргумента нет (вызывающий
	 *  печатает таблицу) либо он не число. */
	bool ParseIndex(const TArray<FString>& Args, int32 MaxIndex, int32& OutIndex, FOutputDevice& Ar)
	{
		if (Args.Num() == 0)
		{
			return false;
		}

		if (!Args[0].IsNumeric())
		{
			Ar.Logf(ELogVerbosity::Warning, TEXT("CA.*: '%s' - не число"), *Args[0]);
			return false;
		}

		OutIndex = FCString::Atoi(*Args[0]);
		if (OutIndex < 0 || OutIndex >= MaxIndex)
		{
			Ar.Logf(ELogVerbosity::Warning, TEXT("CA.*: индекс %d вне диапазона 0..%d"), OutIndex, MaxIndex - 1);
			return false;
		}
		return true;
	}

	void RulePresetCommand(const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
	{
		AAutomataOrchestrator* Orchestrator = FindOrchestrator(Ar);
		if (!Orchestrator)
		{
			return;
		}

		const TArray<FRulePreset> Presets = Orchestrator->GetRulePresets();
		int32 Index = 0;
		if (!ParseIndex(Args, Presets.Num(), Index, Ar))
		{
			Ar.Logf(TEXT("CA.RulePreset: пресеты правил (%d):"), Presets.Num());
			for (int32 It = 0; It < Presets.Num(); ++It)
			{
				Ar.Logf(TEXT("  %2d  %-28s %s"), It, *Presets[It].Name, *Presets[It].RuleString);
			}
			return;
		}

		Orchestrator->ApplyRulePreset(Index);
	}

	void CellShapeCommand(const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
	{
		AAutomataOrchestrator* Orchestrator = FindOrchestrator(Ar);
		if (!Orchestrator)
		{
			return;
		}

		const TArray<FCellShapePreset> Presets = Orchestrator->GetCellShapePresets();
		int32 Index = 0;
		if (!ParseIndex(Args, Presets.Num(), Index, Ar))
		{
			Ar.Logf(TEXT("CA.CellShape: формы клетки (%d):"), Presets.Num());
			for (int32 It = 0; It < Presets.Num(); ++It)
			{
				// Состояние слота меша печатается прямо здесь: пустой слот -
				// самая частая причина того, что форма переключилась, а картинка
				// осталась кубической, и искать это в логе применения дольше,
				// чем увидеть в списке.
				const UStaticMesh* SlotMesh = Orchestrator->GetCellMeshForShape(Presets[It].Shape);
				Ar.Logf(TEXT("  %c%2d  %-32s граней: %2d  меш: %s"),
					Orchestrator->CellShape == Presets[It].Shape ? TEXT('*') : TEXT(' '),
					It, *Presets[It].Name, Presets[It].FaceCount,
					SlotMesh ? *SlotMesh->GetName() : TEXT("СЛОТ ПУСТ"));
			}
			return;
		}

		Orchestrator->ApplyCellShapePreset(Index);
	}

	void GeneratorCommand(const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
	{
		AAutomataOrchestrator* Orchestrator = FindOrchestrator(Ar);
		if (!Orchestrator)
		{
			return;
		}

		const TArray<FStateGeneratorPreset> Presets = Orchestrator->GetStateGeneratorPresets();
		int32 Index = 0;
		if (!ParseIndex(Args, Presets.Num(), Index, Ar))
		{
			Ar.Logf(TEXT("CA.Generator: пресеты генераторов (%d); второй аргумент 1 - сразу построить:"), Presets.Num());
			for (int32 It = 0; It < Presets.Num(); ++It)
			{
				Ar.Logf(TEXT("  %2d  %-28s [%s]"), It, *Presets[It].Name, *Presets[It].FamilyName);
			}
			return;
		}

		// Второй аргумент - строить ли сразу. По умолчанию нет, как и у кнопки в
		// HUD: пресет генератора это отправная точка для правки, а построение -
		// отдельное действие (Y).
		const bool bGenerateNow = Args.Num() > 1 && Args[1] != TEXT("0");
		Orchestrator->ApplyStateGeneratorPreset(Index, bGenerateNow);
	}

	void RenderPresetCommand(const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
	{
		AAutomataOrchestrator* Orchestrator = FindOrchestrator(Ar);
		if (!Orchestrator)
		{
			return;
		}

		const TArray<FRenderPreset> Presets = Orchestrator->GetRenderPresets();
		int32 Index = 0;
		if (!ParseIndex(Args, Presets.Num(), Index, Ar))
		{
			Ar.Logf(TEXT("CA.RenderPreset: профили рендера (%d), они же F1-F4:"), Presets.Num());
			for (int32 It = 0; It < Presets.Num(); ++It)
			{
				Ar.Logf(TEXT("  %2d  %s"), It, *Presets[It].Name);
			}
			return;
		}

		Orchestrator->ApplyRenderPreset(Index);
	}

	void RuleCommand(const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
	{
		AAutomataOrchestrator* Orchestrator = FindOrchestrator(Ar);
		if (!Orchestrator)
		{
			return;
		}

		if (Args.Num() == 0)
		{
			// Печатается ДЕЙСТВУЮЩЕЕ правило (GetActiveRuleString()), а не поле
			// RuleString: то - поле ввода и хранит последнее напечатанное, что
			// после пресета или правки массивов описывает уже другой автомат.
			Ar.Logf(TEXT("CA.Rule: текущее правило %s"), *Orchestrator->GetActiveRuleString());
			Ar.Logf(TEXT("CA.Rule: формат Survival/Birth/States/Neighborhood, например 3,4/3/2/PM"));
			return;
		}

		// Аргументы склеиваются обратно: консоль режет строку по пробелам, а в
		// правиле они допустимы ("5, 7/6/2/M" - законная запись).
		const FString RuleText = FString::Join(Args, TEXT(""));

		FString Error;
		if (!Orchestrator->TryApplyRuleString(RuleText, Error))
		{
			Ar.Logf(ELogVerbosity::Warning, TEXT("CA.Rule: %s"), *Error);
			return;
		}

		Ar.Logf(TEXT("CA.Rule: применено %s"), *Orchestrator->GetActiveRuleString());
	}

	void LightPresetCommand(const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
	{
		AAutomataOrchestrator* Orchestrator = FindOrchestrator(Ar);
		if (!Orchestrator)
		{
			return;
		}

		const TArray<FLightPreset> Presets = Orchestrator->GetLightPresets();
		int32 Index = 0;
		if (!ParseIndex(Args, Presets.Num(), Index, Ar))
		{
			Ar.Logf(TEXT("CA.Light: световые пресеты (%d):"), Presets.Num());
			for (int32 It = 0; It < Presets.Num(); ++It)
			{
				Ar.Logf(TEXT("  %2d  %-12s солнце: %-4s студия: %s"),
					It, *Presets[It].Name,
					Presets[It].bSunEnabled ? TEXT("вкл") : TEXT("выкл"),
					Presets[It].bStudioEnabled ? TEXT("вкл") : TEXT("выкл"));
			}
			return;
		}

		Orchestrator->ApplyLightPreset(Index);
		Ar.Logf(TEXT("CA.Light: применён '%s'"), *Presets[Index].Name);
	}

	/** CA.Panorama [ширина] [поправка экспозиции] - снять сферическую панораму.
	 *
	 *  Единственная в семействе команда, которая ДЕЙСТВУЕТ при пустом списке
	 *  аргументов, а не печатает таблицу. Расхождение с соседями осознанное: у
	 *  тех аргумент - индекс в таблице, которую иначе негде посмотреть, а здесь
	 *  аргументов нет вовсе - есть настройки, и они видны в Details-панели. Зато
	 *  "снять с текущими" - ровно то, ради чего команду и набирают.
	 *
	 *  Аргументы, если заданы, ЗАПИСЫВАЮТСЯ в настройки, а не действуют разово:
	 *  панораму снимают подряд, подбирая размер и экспозицию, и значение,
	 *  откатывающееся после каждого снимка, пришлось бы набирать заново каждый
	 *  раз. */
	void PanoramaCommand(const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
	{
		AAutomataOrchestrator* Orchestrator = FindOrchestrator(Ar);
		if (!Orchestrator)
		{
			return;
		}

		if (Args.Num() > 0)
		{
			if (!Args[0].IsNumeric())
			{
				Ar.Logf(ELogVerbosity::Warning, TEXT("CA.Panorama: '%s' - не число (ожидается ширина в пикселях)"), *Args[0]);
				return;
			}
			Orchestrator->PanoramaWidth = FMath::Clamp(FCString::Atoi(*Args[0]), 512, 16384);
		}

		if (Args.Num() > 1)
		{
			if (!Args[1].IsNumeric())
			{
				Ar.Logf(ELogVerbosity::Warning, TEXT("CA.Panorama: '%s' - не число (ожидается поправка экспозиции, EV)"), *Args[1]);
				return;
			}
			Orchestrator->PanoramaExposureBias = FCString::Atof(*Args[1]);
		}

		// Что снимаем - в консоль ДО съёмки: сама она пишет итог в лог, а человек
		// смотрит сюда, и без этой строки непонятно, какие настройки поехали в
		// дело (особенно после команды с аргументами, которая их поменяла).
		Ar.Logf(TEXT("CA.Panorama: %dx%d, суперсэмпл x%d, экспозиция %+.1f EV - снимаю"),
			Orchestrator->PanoramaWidth, Orchestrator->PanoramaWidth / 2,
			Orchestrator->PanoramaSupersample, Orchestrator->PanoramaExposureBias);

		Orchestrator->TakePanoramaShot();
	}
}

static FAutoConsoleCommand CA_RulePresetCommand(
	TEXT("CA.RulePreset"),
	TEXT("Применить пресет правила по индексу. Без аргумента - список пресетов."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&AutomataConsole::RulePresetCommand));

static FAutoConsoleCommand CA_CellShapeCommand(
	TEXT("CA.CellShape"),
	TEXT("Применить форму клетки по индексу (решётка, окрестность, масштаб меша). Без аргумента - список."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&AutomataConsole::CellShapeCommand));

static FAutoConsoleCommand CA_GeneratorCommand(
	TEXT("CA.Generator"),
	TEXT("Применить пресет генератора: CA.Generator <индекс> [1 - сразу построить]. Без аргумента - список."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&AutomataConsole::GeneratorCommand));

static FAutoConsoleCommand CA_RenderPresetCommand(
	TEXT("CA.RenderPreset"),
	TEXT("Применить профиль рендера по индексу (то же, что F1-F4). Без аргумента - список."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&AutomataConsole::RenderPresetCommand));

static FAutoConsoleCommand CA_RuleCommand(
	TEXT("CA.Rule"),
	TEXT("Задать правило строкой: CA.Rule 3,4/3/2/PM. Без аргумента - показать действующее."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&AutomataConsole::RuleCommand));

static FAutoConsoleCommand CA_LightCommand(
	TEXT("CA.Light"),
	TEXT("Применить световой пресет по индексу (солнце/студия). Без аргумента - список."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&AutomataConsole::LightPresetCommand));

static FAutoConsoleCommand CA_PanoramaCommand(
	TEXT("CA.Panorama"),
	TEXT("Снять сферическую панораму (то же, что Shift+F10): CA.Panorama [ширина] [поправка экспозиции, EV]. Без аргументов - с текущими настройками."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&AutomataConsole::PanoramaCommand));
