// Fill out your copyright notice in the Description page of Project Settings.

#include "Core/PlayerController/GamePlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Core/CameraManager/GameCameraManager.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Orchestration/AutomataOrchestrator.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "Components/PointLightComponent.h"
#include "Engine/LocalPlayer.h"
// Нужен OnSelectDragFinished() - ULocalPlayer::ViewportClient разыменовывается
// ради Viewport. В unity-сборке заголовок приходил транзитивно из соседнего
// файла, поэтому пропажа всплывала только при отдельной компиляции этого файла.
#include "Engine/GameViewportClient.h"
#include "SceneView.h"

AGamePlayerController::AGamePlayerController()
{
	// Ортопроекцию задаёт камера-менеджер (см. AGameCameraManager - там же
	// объяснено, почему не UCameraComponent на пешке). Класс подменяется здесь,
	// а не в BeginPlay(): APlayerController спавнит менеджер в
	// PostInitializeComponents(), то есть раньше.
	PlayerCameraManagerClass = AGameCameraManager::StaticClass();
}

namespace
{
	using FHotkeyHandler = void (AGamePlayerController::*)();

	/** Один обработчик и событие, по которому он зовётся. Событий на действие
	 *  бывает два: нажал/отпустил (F, Shift, ЛКМ) либо "повторять, пока
	 *  держат" плюс отдельная реакция на само нажатие (T/G). */
	struct FHotkeyBinding
	{
		ETriggerEvent Event;
		FHotkeyHandler Handler;
	};

	/** Строка таблицы хоткеев - одно действие целиком: как называется, на каких
	 *  клавишах и что зовёт.
	 *
	 *  Таблицей, а не тремя проходами (создать действие -> смаппить клавишу ->
	 *  привязать обработчик), потому что раньше это и было тремя проходами,
	 *  разъехавшимися на полторы сотни строк: чтобы узнать, что делает клавиша,
	 *  приходилось искать её в маппинге, оттуда идти к действию, а от действия -
	 *  к биндингу. Рассинхрон между ними ловился только вручную; на профилях
	 *  рендера пришлось даже завести static_assert, что клавиш столько же,
	 *  сколько действий, - в таблице такой инвариант выразить нечем, потому что
	 *  нечему разъехаться. Тот же приём, что у RulePresets/RenderPresets/
	 *  CapturePresets: таблица-константа и один проход по ней.
	 *
	 *  Клавиши берутся не из Content-ассетов - см. doc-comment InputActions. */
	struct FHotkeyRow
	{
		const TCHAR* ActionName;
		/** Не клавиши, а действия из реестра - сама клавиша берётся через
		 *  KeyFor(), т.е. из Config/DefaultInput.ini с падением на значение по
		 *  умолчанию. Два элемента там, где у действия две клавиши (скорость на
		 *  основном ряду и на нумпаде). */
		TArray<EHotkey> Hotkeys;
		TArray<FHotkeyBinding> Bindings;
	};
}

FKey AGamePlayerController::KeyFor(EHotkey Hotkey) const
{
	const int32 Index = (int32)Hotkey;
	if (ResolvedHotkeys.IsValidIndex(Index))
	{
		return ResolvedHotkeys[Index];
	}

	// До SetupInputComponent() (или если реестр почему-то не сошёлся по длине) -
	// значение по умолчанию: пустая клавиша означала бы "хоткея нет", а это
	// худший из возможных ответов на вопрос "какая клавиша у этого действия".
	const TArray<HotkeyRegistry::FHotkeyDefault>& Defaults = HotkeyRegistry::GetDefaults();
	return Defaults.IsValidIndex(Index) ? Defaults[Index].DefaultKey : EKeys::Invalid;
}

void AGamePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// Пробел (пауза), R (сброс) и N (новый сид) намеренно не в таблице - см.
	// InputKey() ниже, все три перехватываются на уровне сырых оконных событий
	// в обход Enhanced Input (у R и N тот же лаговый баг пропущенного нажатия,
	// что был у паузы - см. doc-comment InputKey()).
	//
	// Про ETriggerEvent в строках ниже: Started - однократно на нажатие,
	// Completed - на отпускание, Triggered - каждый кадр удержания. Triggered
	// стоит там, где величину подбирают на глаз (скорость, срез, сдвиг куба), и
	// обработчик сам прореживает поток до частоты автоповтора - см.
	// ShouldFireRepeat(). Started - там, где повтор на кадре был бы вреден:
	// профиль рендера переприменялся бы полным RenderGridImmediate() каждый
	// кадр, а удвоение StepsPerRender улетело бы в потолок мгновенно.
	const TArray<FHotkeyRow> Hotkeys =
	{
		// F - тумблер (обычное нажатие) или hold-режим (Shift+F), а не
		// "срабатывает каждый кадр, пока зажата", как было раньше.
		{ TEXT("IA_FastStep"), { EHotkey::FastStep }, {
			{ ETriggerEvent::Started, &AGamePlayerController::OnFastStepPressed },
			{ ETriggerEvent::Completed, &AGamePlayerController::OnFastStepReleased } } },

		// F1-F4, а не 1-4: цифровой ряд отдан фильтру по возрасту (см.
		// InputKey()), а возрастные слои перебирают постоянно при осмотре,
		// тогда как профиль рендера ставят изредка.
		//
		// Отдельная клавиша на профиль, а не одна циклическая: профилей четыре,
		// и "сделать быстро" нужно немедленно, а не после трёх нажатий вслепую -
		// перебор имеет смысл там, где вариантов много и они равноправны
		// (ChunkedRenderOrder на X), а не там, где есть явные "как задумано" и
		// "максимально быстро" на краях списка.
		//
		// Их ровно четыре, и это НЕ размер таблицы RenderPresets::GetAll() -
		// профилей там пять: последний, Photo, клавиши не имеет намеренно, его
		// применяет сама съёмка (TakePhotoShot() на F10), потому что вне снимка
		// он не нужен. Профилю, который добавят для повседневной работы, нужна
		// одна новая строка здесь - вместе с клавишей и обработчиком.
		{ TEXT("IA_RenderPreset0"), { EHotkey::RenderPreset0 }, { { ETriggerEvent::Started, &AGamePlayerController::OnApplyRenderPreset0 } } },
		{ TEXT("IA_RenderPreset1"), { EHotkey::RenderPreset1 }, { { ETriggerEvent::Started, &AGamePlayerController::OnApplyRenderPreset1 } } },
		{ TEXT("IA_RenderPreset2"), { EHotkey::RenderPreset2 }, { { ETriggerEvent::Started, &AGamePlayerController::OnApplyRenderPreset2 } } },
		{ TEXT("IA_RenderPreset3"), { EHotkey::RenderPreset3 }, { { ETriggerEvent::Started, &AGamePlayerController::OnApplyRenderPreset3 } } },

		{ TEXT("IA_ToggleBackground"), { EHotkey::ToggleBackground }, { { ETriggerEvent::Started, &AGamePlayerController::OnToggleBackground } } },

		{ TEXT("IA_SpeedBoost"), { EHotkey::SpeedBoost }, {
			{ ETriggerEvent::Started, &AGamePlayerController::OnSpeedBoostStarted },
			{ ETriggerEvent::Completed, &AGamePlayerController::OnSpeedBoostEnded } } },

		{ TEXT("IA_ToggleChunkedRender"), { EHotkey::ToggleChunkedRender }, { { ETriggerEvent::Started, &AGamePlayerController::OnToggleChunkedRender } } },
		{ TEXT("IA_CycleChunkedRenderOrder"), { EHotkey::CycleChunkedRenderOrder }, { { ETriggerEvent::Started, &AGamePlayerController::OnCycleChunkedRenderOrder } } },
		{ TEXT("IA_ToggleWaitForChunkedRenderToFinish"), { EHotkey::ToggleWaitForChunkedRenderToFinish }, { { ETriggerEvent::Started, &AGamePlayerController::OnToggleWaitForChunkedRenderToFinish } } },
		{ TEXT("IA_ToggleCellCulling"), { EHotkey::ToggleCellCulling }, { { ETriggerEvent::Started, &AGamePlayerController::OnToggleCellCulling } } },
		{ TEXT("IA_ToggleRenderCullVolume"), { EHotkey::ToggleRenderCullVolume }, { { ETriggerEvent::Started, &AGamePlayerController::OnToggleRenderCullVolume } } },
		{ TEXT("IA_ToggleGhostShape"), { EHotkey::ToggleGhostShape }, { { ETriggerEvent::Started, &AGamePlayerController::OnToggleGhostShape } } },

		// Основной ряд (=/-) и NumPad (+/-) - чтобы работало независимо от
		// того, есть ли у клавиатуры цифровой блок.
		{ TEXT("IA_IncreaseSpeed"), { EHotkey::IncreaseSpeed, EHotkey::IncreaseSpeedNumPad }, { { ETriggerEvent::Triggered, &AGamePlayerController::OnIncreaseSpeed } } },
		{ TEXT("IA_DecreaseSpeed"), { EHotkey::DecreaseSpeed, EHotkey::DecreaseSpeedNumPad }, { { ETriggerEvent::Triggered, &AGamePlayerController::OnDecreaseSpeed } } },

		{ TEXT("IA_FrameAllCells"), { EHotkey::FrameAllCells }, { { ETriggerEvent::Started, &AGamePlayerController::OnFrameAllCells } } },

		// Одна клавиша, два события: Triggered повторяет шаг, пока держат, а
		// Started под Shift переходит к следующей степени двойки. Какой из двух
		// обработчиков сработает, решает проверка Shift внутри них самих -
		// Enhanced Input не умеет требовать модификатор в маппинге клавиши.
		{ TEXT("IA_IncreaseStepsPerRender"), { EHotkey::IncreaseStepsPerRender }, {
			{ ETriggerEvent::Triggered, &AGamePlayerController::OnIncreaseStepsPerRender },
			{ ETriggerEvent::Started, &AGamePlayerController::OnDoubleStepsPerRender } } },
		{ TEXT("IA_DecreaseStepsPerRender"), { EHotkey::DecreaseStepsPerRender }, {
			{ ETriggerEvent::Triggered, &AGamePlayerController::OnDecreaseStepsPerRender },
			{ ETriggerEvent::Started, &AGamePlayerController::OnHalveStepsPerRender } } },

		// Стрелки заняты ADefaultPawn (полёт и поворот), поэтому обработчики
		// работают только в режиме выделения, где ввод пешки отключён - см.
		// OnMoveCullVolume().
		{ TEXT("IA_MoveCullVolumeUp"), { EHotkey::MoveCullVolumeUp }, { { ETriggerEvent::Triggered, &AGamePlayerController::OnMoveCullVolumeUp } } },
		{ TEXT("IA_MoveCullVolumeDown"), { EHotkey::MoveCullVolumeDown }, { { ETriggerEvent::Triggered, &AGamePlayerController::OnMoveCullVolumeDown } } },
		{ TEXT("IA_MoveCullVolumeLeft"), { EHotkey::MoveCullVolumeLeft }, { { ETriggerEvent::Triggered, &AGamePlayerController::OnMoveCullVolumeLeft } } },
		{ TEXT("IA_MoveCullVolumeRight"), { EHotkey::MoveCullVolumeRight }, { { ETriggerEvent::Triggered, &AGamePlayerController::OnMoveCullVolumeRight } } },

		{ TEXT("IA_ToggleViewSlice"), { EHotkey::ToggleViewSlice }, { { ETriggerEvent::Started, &AGamePlayerController::OnToggleViewSlice } } },
		// [ и ] освободились, когда StepsPerRender переехал на T/G.
		{ TEXT("IA_ViewSliceNearer"), { EHotkey::ViewSliceNearer }, { { ETriggerEvent::Triggered, &AGamePlayerController::OnViewSliceNearer } } },
		{ TEXT("IA_ViewSliceFarther"), { EHotkey::ViewSliceFarther }, { { ETriggerEvent::Triggered, &AGamePlayerController::OnViewSliceFarther } } },

		{ TEXT("IA_ToggleSelectionMode"), { EHotkey::ToggleSelectionMode }, { { ETriggerEvent::Started, &AGamePlayerController::OnToggleSelectionMode } } },
		{ TEXT("IA_SelectDrag"), { EHotkey::SelectDrag }, {
			{ ETriggerEvent::Started, &AGamePlayerController::OnSelectDragStarted },
			{ ETriggerEvent::Completed, &AGamePlayerController::OnSelectDragFinished } } },
		// ПКМ живёт только внутри режима рисования (обработчик выходит сразу,
		// если он выключен) - вне его правая кнопка проекту не нужна, а пешке
		// она и так не назначена. Только Started: клетка убирается нажатием,
		// отпускание ничего не значит.
		{ TEXT("IA_EraseCell"), { EHotkey::EraseCell }, { { ETriggerEvent::Started, &AGamePlayerController::OnEraseCellPressed } } },
		{ TEXT("IA_ExtractSelection"), { EHotkey::ExtractSelection }, { { ETriggerEvent::Started, &AGamePlayerController::OnExtractSelection } } },
		{ TEXT("IA_InvertSelection"), { EHotkey::InvertSelection }, { { ETriggerEvent::Started, &AGamePlayerController::OnInvertSelection } } },
		{ TEXT("IA_BakeCellsToMesh"), { EHotkey::BakeCellsToMesh }, { { ETriggerEvent::Started, &AGamePlayerController::OnBakeCellsToMesh } } },
		{ TEXT("IA_DeleteSelectedCells"), { EHotkey::DeleteSelectedCells }, { { ETriggerEvent::Started, &AGamePlayerController::OnDeleteSelectedCells } } },
		{ TEXT("IA_MoveCullVolumeToSelection"), { EHotkey::MoveCullVolumeToSelection }, { { ETriggerEvent::Started, &AGamePlayerController::OnMoveCullVolumeToSelection } } },
		{ TEXT("IA_SelectCellsInCullVolume"), { EHotkey::SelectCellsInCullVolume }, { { ETriggerEvent::Started, &AGamePlayerController::OnSelectCellsInCullVolume } } },

		// S/O замапплены БЕЗ модификатора - Enhanced Input не даёт потребовать
		// Ctrl прямо в маппинге ключа (в отличие от старых FInputChord).
		// Ctrl(+Shift) проверяется внутри OnSaveOrSaveAs()/OnLoadState() - та же
		// идиома, что у Ctrl/Shift в OnSelectDragStarted(). Голый S по-прежнему
		// уходит камере (DefaultPawn, движение назад) - это осознанный побочный
		// эффект удержания Ctrl+S во время полёта, см. doc-comment.
		{ TEXT("IA_SaveState"), { EHotkey::SaveState }, { { ETriggerEvent::Started, &AGamePlayerController::OnSaveOrSaveAs } } },
		{ TEXT("IA_LoadState"), { EHotkey::LoadState }, { { ETriggerEvent::Started, &AGamePlayerController::OnLoadState } } },
		// D - та же схема, что у S/O: маппинг без модификатора, Ctrl проверяется
		// внутри обработчика, голая D остаётся движением камеры вправо.
		{ TEXT("IA_ArrayCells"), { EHotkey::ArrayCells }, { { ETriggerEvent::Started, &AGamePlayerController::OnArrayCells } } },
		// C и V уже заняты голыми (отсечение и ожидание чанкового рендера) -
		// это ВТОРЫЕ действия на тех же клавишах, отобранные модификатором в
		// самих обработчиках. Прежние обработчики, в свою очередь, отсеивают
		// нажатие с модификатором, чтобы одно нажатие не сделало обе вещи.
		{ TEXT("IA_CopyCells"), { EHotkey::CopyCells }, { { ETriggerEvent::Started, &AGamePlayerController::OnCopyCells } } },
		{ TEXT("IA_PasteCells"), { EHotkey::PasteCells }, { { ETriggerEvent::Started, &AGamePlayerController::OnPasteCells } } },

		// Поворот буфера перед вставкой. Started, а не Triggered: поворот на 90
		// градусов - дискретный шаг, и автоповтор при удержании прокручивал бы
		// ориентацию мимо нужной. Все шесть работают только в режиме рисования;
		// стрелки в это время у куба отсечения простаивают (его обработчики
		// гейтятся режимом выделения).
		{ TEXT("IA_RotateClipboardYawLeft"), { EHotkey::RotateClipboardYawLeft }, { { ETriggerEvent::Started, &AGamePlayerController::OnRotateClipboardYawLeft } } },
		{ TEXT("IA_RotateClipboardYawRight"), { EHotkey::RotateClipboardYawRight }, { { ETriggerEvent::Started, &AGamePlayerController::OnRotateClipboardYawRight } } },
		{ TEXT("IA_RotateClipboardPitchUp"), { EHotkey::RotateClipboardPitchUp }, { { ETriggerEvent::Started, &AGamePlayerController::OnRotateClipboardPitchUp } } },
		{ TEXT("IA_RotateClipboardPitchDown"), { EHotkey::RotateClipboardPitchDown }, { { ETriggerEvent::Started, &AGamePlayerController::OnRotateClipboardPitchDown } } },
		{ TEXT("IA_RotateClipboardRollLeft"), { EHotkey::RotateClipboardRollLeft }, { { ETriggerEvent::Started, &AGamePlayerController::OnRotateClipboardRollLeft } } },
		{ TEXT("IA_RotateClipboardRollRight"), { EHotkey::RotateClipboardRollRight }, { { ETriggerEvent::Started, &AGamePlayerController::OnRotateClipboardRollRight } } },
	};

	// Раскладка разрешается ОДИН раз здесь, и дальше только читается: и таблица
	// ниже, и InputKey() спрашивают клавишу через KeyFor(). Это то место, где
	// Config/DefaultInput.ini вступает в силу - см. HotkeyRegistry.
	ResolvedHotkeys = HotkeyRegistry::ResolveKeys();

	// Конфликт в ini не даёт ни ошибки компиляции, ни отказа - обе клавиши
	// просто срабатывают вместе, а выглядит это как "хоткей делает что-то
	// лишнее". Поэтому его хотя бы видно в логе.
	for (const TPair<FKey, TArray<FName>>& Conflict : HotkeyRegistry::FindConflicts(ResolvedHotkeys))
	{
		TArray<FString> Names;
		for (const FName& Name : Conflict.Value)
		{
			Names.Add(Name.ToString());
		}
		UE_LOG(LogTemp, Warning, TEXT("Хоткеи: клавиша %s назначена нескольким действиям: %s"),
			*Conflict.Key.ToString(), *FString::Join(Names, TEXT(", ")));
	}

	SimulationMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Simulation"));
	UEnhancedInputComponent* EnhancedInputComp = Cast<UEnhancedInputComponent>(InputComponent);

	InputActions.Reset(Hotkeys.Num());
	for (const FHotkeyRow& Row : Hotkeys)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Row.ActionName);
		// Все хоткеи проекта булевы: ни один не читает величину нажатия, а
		// повтор при удержании даёт сам ETriggerEvent::Triggered.
		Action->ValueType = EInputActionValueType::Boolean;
		InputActions.Add(Action);

		for (EHotkey Hotkey : Row.Hotkeys)
		{
			SimulationMappingContext->MapKey(Action, KeyFor(Hotkey));
		}

		if (EnhancedInputComp)
		{
			for (const FHotkeyBinding& Binding : Row.Bindings)
			{
				EnhancedInputComp->BindAction(Action, Binding.Event, this, Binding.Handler);
			}
		}
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		Subsystem->AddMappingContext(SimulationMappingContext, 0);
	}
}

bool AGamePlayerController::InputKey(const FInputKeyEventArgs& Params)
{
	// Пауза/продолжение - на пробеле, а не на P: это самая частая клавиша
	// проекта, и ей место под большим пальцем. Пробел был занят подъёмом камеры
	// и освобождён в RebindPawnVerticalMovement() - вертикаль осталась на E
	// (вверх) и Q (вниз), т.е. потеряно дублирование, а не сама возможность.
	//
	// Enhanced Input оценивает свои триггеры (Started/Triggered/...) раз за
	// кадр, по текущему состоянию клавиши "нажата сейчас или нет" - если
	// игровой поток лагает (тяжёлый AddInstances/перестройка HISM-дерева при
	// большом числе клеток), один кадр может растянуться настолько, что
	// короткое нажатие+отпускание пробела целиком уместится между двумя такими
	// выборками и не будет замечено вообще - пауза "не срабатывает", и чем
	// сильнее лаг, тем чаще. InputKey() же вызывается немедленно на каждое
	// оконное сообщение (WM_KEYDOWN/WM_KEYUP), независимо от длины кадра, до
	// периодической выборки Enhanced Input - поэтому пауза обрабатывается
	// здесь напрямую, в обход маппинга. IE_Pressed (не IE_Repeat) - как и
	// раньше, срабатывает один раз на нажатие, не повторяется при удержании.
	if (Params.Key == KeyFor(EHotkey::ToggleSimulation) && Params.Event == IE_Pressed)
	{
		OnToggleSimulation();
	}

	// R (сброс, OnResetSimulation()) - тот же самый баг и то же решение, что
	// у паузы выше: раньше был замаплен через Enhanced Input и мог пропускать
	// короткие нажатия под тяжёлым лагом (пользователь сообщил "не всегда
	// срабатывает" - именно этот симптом). Перенесён сюда, в обход маппинга.
	if (Params.Key == KeyFor(EHotkey::ResetSimulation) && Params.Event == IE_Pressed)
	{
		OnResetSimulation();
	}

	// N (новый сид, OnNewSeed()) - третий случай того же бага: реролл нажимают
	// именно тогда, когда картинка не нравится и сетка уже разрослась, т.е.
	// ровно в момент худшего лага, когда выборка Enhanced Input раз в кадр
	// пропускает короткие нажатия. Второй, независимый источник того же
	// симптома - молчаливый отказ GenerateRandom() во время фонового шага -
	// закрыт отложенным путём на стороне оркестратора (см. bNewSeedPending).
	if (Params.Key == KeyFor(EHotkey::NewSeed) && Params.Event == IE_Pressed)
	{
		OnNewSeed();
	}

	// Y (построить состояние генератором, Shift+Y - следующий тип, Ctrl+Y -
	// гистограмма соседей живой структуры) - тот же
	// случай, что пауза/R/N: генератор нажимают ровно тогда, когда картинка не
	// нравится и сетка уже разрослась, то есть в момент худшего лага.
	// Модификатор проверяется внутри обработчика, а не маппингом - Enhanced
	// Input не умеет требовать модификатор в привязке клавиши (та же идиома,
	// что у Ctrl+S/Ctrl+C).
	if (Params.Key == KeyFor(EHotkey::GenerateState) && Params.Event == IE_Pressed)
	{
		OnGenerateState();
	}

	// Клавиши полёта в режиме выделения (Tab) - выходим из режима, а не молчим.
	// В нём ввод пешки отключён (SetCameraControlEnabled(false)), поэтому W/A/S/D
	// и Q/E раньше просто не делали ничего, а жмут их ровно тогда, когда с
	// выделением и кубом закончили и хотят лететь дальше - лишний Tab был чистой
	// формальностью. Нажатие не съедается: ось движения опрашивается по
	// состоянию клавиши каждый кадр, так что уже зажатая клавиша поедет сама,
	// без повторного нажатия.
	//
	// Ctrl исключён: Ctrl+S - это сохранение (см. OnSaveOrSaveAs()), и оно не
	// должно попутно выбрасывать из режима. Shift не исключён - это ускорение
	// полёта, то есть то же самое движение.
	const bool bCtrlDown = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);

	// Ctrl+Z - отмена последнего действия (AAutomataOrchestrator::
	// UndoLastAction(): ручная правка снимается дельтой, шаг симуляции -
	// пересчётом), Ctrl+Shift+Z - повтор отменённой правки. Голая Z остаётся
	// за Enhanced Input'ом (чанковый рендер) - тот сам отсеивает нажатие с
	// Ctrl, потому что выразить "без модификатора" в привязке нельзя (см.
	// OnToggleChunkedRender()).
	//
	// IE_Pressed, а не IE_Repeat, и это не мелочь: отмена шага стоит полного
	// пересчёта от изначального узора, так что авторепит на зажатой клавише
	// сложился бы в сумму всех номеров поколений, то есть в квадрат.
	if (Params.Key == KeyFor(EHotkey::UndoRedo) && Params.Event == IE_Pressed && bCtrlDown)
	{
		if (IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift))
		{
			OnRedoEdit();
		}
		else
		{
			OnUndoLastAction();
		}
	}

	if ((bSelectionModeActive || bDrawModeActive) && Params.Event == IE_Pressed && !bCtrlDown)
	{
		// Единственные клавиши, не заведённые в HotkeyRegistry, и намеренно:
		// это не хоткеи проекта, а клавиши движения ПЕШКИ (ADefaultPawn плюс
		// вертикаль из RebindPawnVerticalMovement()). Их настройка живёт там же,
		// где сама привязка осей, и продублировать их здесь отдельной строкой
		// конфига значило бы завести вторую правду о том, чем летают: разъехались
		// бы - и выход из режима перестал бы срабатывать ровно на тех клавишах,
		// которыми на самом деле летят.
		static const FKey FlyKeys[] = {
			EKeys::W, EKeys::A, EKeys::S, EKeys::D, EKeys::Q, EKeys::E
		};

		for (const FKey& FlyKey : FlyKeys)
		{
			if (Params.Key == FlyKey)
			{
				SetSelectionModeActive(false);
				// И рисование тоже: причина та же - в режиме ввод пешки
				// отключён, так что эти клавиши иначе не делают ничего ровно
				// тогда, когда хочется улететь от нарисованного.
				SetDrawModeActive(false);
				break;
			}
		}
	}

	// F5 - показать/скрыть информационную панель HUD. Здесь, а не через
	// Enhanced Input, за компанию с соседними клавишами: F1-F4 (профили
	// рендера) идут через маппинг, но эта клавиша ничего не ждёт от триггеров
	// и одному нажатию должна соответствовать ровно одна реакция.
	if (Params.Key == KeyFor(EHotkey::ToggleHudInfoPanel) && Params.Event == IE_Pressed)
	{
		OnToggleHudInfoPanel();
	}

	// F10 - парадный снимок в максимальном разрешении. Именно F10, а не
	// соседняя свободная клавиша: из F-ряда движок занимает под свои
	// DebugExecBindings F1-F5, F9 и F11, F8 в PIE выбрасывает из пешки, а F6/F7
	// уже наши (срез и серия). F10 - единственная, не занятая никем.
	if (Params.Key == KeyFor(EHotkey::TakePhotoShot) && Params.Event == IE_Pressed)
	{
		OnTakePhotoShot();
	}

	// P - звук вкл/выкл, Shift+P - следующий набор настроек сонификации.
	// Единственная свободная буква алфавита: освободилась, когда пауза уехала
	// на пробел. Здесь, а не через Enhanced Input, по той же причине, что и
	// соседи: одному нажатию - ровно одна реакция, и модификатор проверяется
	// внутри обработчика, потому что маппинг требовать его не умеет.
	if (Params.Key == KeyFor(EHotkey::ToggleSonification) && Params.Event == IE_Pressed)
	{
		OnToggleSonification();
	}

	// F6 - снять текущий вид как PNG-срез, Shift+F6 - то же с диалогом выбора
	// файла. Модификатор проверяется в обработчике (Enhanced Input не умеет
	// требовать его в привязке), см. OnCaptureTextureSlice().
	if (Params.Key == KeyFor(EHotkey::CaptureTextureSlice) && Params.Event == IE_Pressed)
	{
		OnCaptureTextureSlice();
	}

	// F7 - начать/оборвать съёмку серии кадров по ходу симуляции.
	if (Params.Key == KeyFor(EHotkey::ToggleSeriesCapture) && Params.Event == IE_Pressed)
	{
		OnToggleSeriesCapture();
	}

	// Цифры 0-9 - фильтр по возрасту (9 - ещё и всё, что старше), Shift+цифра -
	// добавить возраст к показанным или убрать его. Здесь, а не через Enhanced
	// Input, по причине, не связанной с лагом: десять клавиш одного вида - это
	// десять UInputAction, десять MapKey и десять обработчиков ради одного
	// switch. Читаемость дороже единообразия, а задержки эти клавиши не
	// критичны, в отличие от паузы/R/N выше.
	if (Params.Event == IE_Pressed)
	{
		// Десять подряд идущих значений реестра, а не таблица клавиш здесь:
		// цифра - это параметр обработчика, и порядок EHotkey::AgeFilter0..9
		// задаёт его напрямую (проверяется тестом Input.HotkeyRegistry).
		for (int32 Digit = 0; Digit < 10; ++Digit)
		{
			if (Params.Key == KeyFor((EHotkey)((int32)EHotkey::AgeFilter0 + Digit)))
			{
				OnSetAgeFilter(Digit);
				break;
			}
		}
	}

	// Нумпад - позиционирование камеры: Home кадрирует по текущему ракурсу, а
	// это то же самое, но с заданной стороны. Тоже в InputKey(), а не через
	// Enhanced Input, по той же причине, что цифры выше: дюжина клавиш одного
	// вида - это дюжина UInputAction ради одной таблицы.
	//
	// Работает при ВКЛЮЧЁННОМ NumLock: без него Windows присылает с этих клавиш
	// Left/Up/Home/Delete и прочее, т.е. коды, которыми двигают куб отсечения.
	// Конфликта нет (это разные FKey), но это ровно то, из-за чего нумпад без
	// NumLock кажется нерабочим.
	if (Params.Event == IE_Pressed)
	{
		// Направление ВЗГЛЯДА (камера ставится с противоположной стороны, см.
		// FrameAllCellsFromDirection()). Оси проекта: X - вперёд, Y - вправо,
		// Z - вверх, поэтому "вид слева" - это взгляд в +Y. Раскладка
		// геометрическая, как в DCC-редакторах: 4 слева, 6 справа, 8 сверху,
		// 2 снизу.
		struct FNumPadViewBinding
		{
			EHotkey Hotkey;
			FVector ViewDirection;
			const TCHAR* Name;
		};
		static const FNumPadViewBinding ViewBindings[] = {
			{ EHotkey::ViewLeft,      FVector( 0.0,  1.0,  0.0), TEXT("слева") },
			{ EHotkey::ViewRight,     FVector( 0.0, -1.0,  0.0), TEXT("справа") },
			{ EHotkey::ViewTop,       FVector( 0.0,  0.0, -1.0), TEXT("сверху") },
			{ EHotkey::ViewBottom,    FVector( 0.0,  0.0,  1.0), TEXT("снизу") },
			{ EHotkey::ViewFront,     FVector( 1.0,  0.0,  0.0), TEXT("спереди") },
			{ EHotkey::ViewBack,      FVector(-1.0,  0.0,  0.0), TEXT("сзади") },
			// Единственный ракурс, показывающий сразу три оси - по нему видно
			// объём структуры, которого осевые виды как раз не показывают.
			{ EHotkey::ViewIsometric, FVector( 1.0,  1.0, -1.0), TEXT("изометрия") },
		};

		if (Params.Key == KeyFor(EHotkey::ToggleOrthographic))
		{
			OnToggleOrthographic();
		}
		else if (Params.Key == KeyFor(EHotkey::FrameAllCellsFromNumPad))
		{
			// Ровно то же, что Home - кадр по текущему ракурсу; на нумпаде
			// нужен потому, что рука уже там.
			OnFrameAllCells();
		}
		else if (Params.Key == KeyFor(EHotkey::AlignCameraToOppositeSide))
		{
			OnAlignCameraToOppositeSide();
		}
		else if (Params.Key == KeyFor(EHotkey::FrameSelection))
		{
			OnFrameSelection();
		}
		else if (Params.Key == KeyFor(EHotkey::OrthoZoomIn))
		{
			OnAdjustOrthoWidth(/*bZoomIn=*/true);
		}
		else if (Params.Key == KeyFor(EHotkey::OrthoZoomOut))
		{
			OnAdjustOrthoWidth(/*bZoomIn=*/false);
		}
		else
		{
			for (const FNumPadViewBinding& Binding : ViewBindings)
			{
				if (Params.Key == KeyFor(Binding.Hotkey))
				{
					// Shift - кадрировать только по видимому (см. doc-comment
					// FrameAllCells()); снят один раз на всю таблицу, а не в
					// каждой ветке отдельно.
					OnAlignCamera(Binding.ViewDirection, Binding.Name, IsShiftHeld());
					break;
				}
			}
		}
	}

	return Super::InputKey(Params);
}

void AGamePlayerController::OnToggleSimulation()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleSimulation: AAutomataOrchestrator не найден в мире"));
		return;
	}

	if (Orchestrator->IsSimulationRunning())
	{
		Orchestrator->Stop();
	}
	else
	{
		Orchestrator->Start();
	}
}

void AGamePlayerController::OnFastStepPressed()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnFastStepPressed: AAutomataOrchestrator не найден в мире"));
		return;
	}

	if (Orchestrator->IsSimulationRunning())
	{
		UE_LOG(LogTemp, Warning, TEXT("OnFastStepPressed: непрерывная симуляция уже идёт (пробел) - F игнорируется"));
		return;
	}

	// Shift+F - непрерывный автошаг "как Play", пока F зажата (см.
	// OnFastStepReleased()); голый F - один шаг, как и раньше.
	if (IsInputKeyDown(EKeys::LeftShift))
	{
		Orchestrator->StartFastStep();
	}
	else
	{
		Orchestrator->Next();
	}
}

void AGamePlayerController::OnFastStepReleased()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (Orchestrator && Orchestrator->IsFastStepActive())
	{
		Orchestrator->StopFastStep();
	}
}

void AGamePlayerController::OnResetSimulation()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnResetSimulation: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->ResetToInitialState();
}

void AGamePlayerController::OnNewSeed()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnNewSeed: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Shift+N - тумблер автоперебора сидов вместо одного реролла: то же самое
	// нажатие N, но за тебя и до тех пор, пока какая-нибудь структура не
	// выживет (см. AAutomataOrchestrator::bAutoReseedOnExtinction). Модификатор
	// проверяется здесь, а не маппингом - клавиша и так ловится в InputKey(), а
	// Enhanced Input всё равно не умеет требовать модификатор в привязке.
	if (IsShiftHeld())
	{
		Orchestrator->SetAutoReseedOnExtinction(!Orchestrator->IsAutoReseedOnExtinction());
		return;
	}

	Orchestrator->NewSeed();
}

void AGamePlayerController::OnGenerateState()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnGenerateState: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Ctrl+Y - гистограмма соседей ЖИВОЙ структуры. Рядом с Y не случайно:
	// Y печатает такую же гистограмму для только что СГЕНЕРИРОВАННОГО набора
	// (см. FStateGeneratorParams::bAnalyzeNeighborCounts), и это буквально та
	// же мера, снятая в другой момент - до эволюции и после.
	if (IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl))
	{
		Orchestrator->AnalyzeLiveStructure();
		return;
	}

	if (IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift))
	{
		Orchestrator->CycleStateGeneratorType();
	}
	else
	{
		Orchestrator->GenerateState();
	}
}

void AGamePlayerController::OnToggleHudInfoPanel()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleHudInfoPanel: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->ToggleHudInfoPanel();
}

void AGamePlayerController::OnToggleSonification()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleSonification: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Shift - перебрать наборы настроек, не трогая сам переключатель: набор
	// имеет смысл менять на слух, прямо во время прогона, и гасить ради этого
	// звук было бы ровно наоборот.
	if (IsShiftHeld())
	{
		Orchestrator->CycleSonificationPreset();
		return;
	}

	Orchestrator->SetSonificationEnabled(!Orchestrator->IsSonificationEnabled());
}

void AGamePlayerController::OnTakePhotoShot()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnTakePhotoShot: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->TakePhotoShot();
}

void AGamePlayerController::OnCaptureTextureSlice()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnCaptureTextureSlice: AAutomataOrchestrator не найден в мире"));
		return;
	}

	if (IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift))
	{
		Orchestrator->CaptureTextureSliceAs();
	}
	else
	{
		Orchestrator->CaptureTextureSlice();
	}
}

void AGamePlayerController::OnToggleSeriesCapture()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleSeriesCapture: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Shift+F7 - выбрать, ЧЕМ снимать (следующий набор настроек по кругу), а не
	// снимать. Пара к F7 ровно как Shift+Y к Y: модификатор выбирает режим,
	// чистое нажатие запускает. Модификатор проверяется здесь, а не маппингом -
	// Enhanced Input не умеет требовать его в привязке клавиши (та же идиома,
	// что у Shift+F6/Ctrl+S).
	if (IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift))
	{
		Orchestrator->CycleCapturePreset();
		return;
	}

	// Одна клавиша на начало и на обрыв - StartSeriesCapture() сам решает, что
	// значит нажатие в текущем состоянии.
	Orchestrator->StartSeriesCapture();
}

void AGamePlayerController::OnApplyRenderPreset(int32 PresetIndex)
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnApplyRenderPreset: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Профиль применяется целиком в оркестраторе, включая VIEWMODE - здесь
	// намеренно не осталось ни одной консольной команды: иначе часть настроек
	// профиля жила бы в контроллере, а часть в оркестраторе, и добавление
	// пятого профиля требовало бы правок в обоих (см. ApplyRenderPreset()).
	Orchestrator->ApplyRenderPreset(PresetIndex);
}

void AGamePlayerController::OnApplyRenderPreset0() { OnApplyRenderPreset(0); }
void AGamePlayerController::OnApplyRenderPreset1() { OnApplyRenderPreset(1); }
void AGamePlayerController::OnApplyRenderPreset2() { OnApplyRenderPreset(2); }
void AGamePlayerController::OnApplyRenderPreset3() { OnApplyRenderPreset(3); }

void AGamePlayerController::OnToggleBackground()
{
	// Shift+U - лампочка на камере, а не фон. Ей оркестратор нужен только ради
	// настроек, поэтому проверка идёт ДО поиска актёра: без оркестратора свет
	// всё равно зажжётся, просто с движковыми значениями яркости и радиуса.
	if (IsShiftHeld())
	{
		SetHeadlightEnabled(!bHeadlightEnabled);
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleBackground: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetBackgroundVisible(!Orchestrator->IsBackgroundVisible());
}

void AGamePlayerController::SetHeadlightEnabled(bool bEnable)
{
	bHeadlightEnabled = bEnable;

	if (!bEnable)
	{
		// Компонент не уничтожается - гасится. Лампочку щёлкают туда-сюда, а
		// пересоздание светового компонента это регистрация в сцене заново.
		if (HeadlightComponent)
		{
			HeadlightComponent->SetVisibility(false);
		}
		ShowCameraStatusMessage(TEXT("Лампочка на камере: выключена"));
		return;
	}

	UpdateHeadlight();

	// Сообщение по факту, а не по флагу: если пешки ещё нет, EnsureHeadlight()
	// вернула nullptr и светить пока нечему - Tick() дожжёт лампочку на
	// следующем кадре сам, но врать про "включена" в этот момент не надо.
	ShowCameraStatusMessage(HeadlightComponent
		? TEXT("Лампочка на камере: включена")
		: TEXT("Лампочка на камере: пешки ещё нет, включится сама"));
}

UPointLightComponent* AGamePlayerController::EnsureHeadlight()
{
	APawn* ControlledPawn = GetPawn();
	if (!ControlledPawn || !ControlledPawn->GetRootComponent())
	{
		return nullptr;
	}

	// Пешка сменилась - прежний свет принадлежит ей и уедет вместе с ней.
	if (HeadlightComponent && HeadlightComponent->GetOwner() != ControlledPawn)
	{
		HeadlightComponent->DestroyComponent();
		HeadlightComponent = nullptr;
	}

	if (IsValid(HeadlightComponent))
	{
		return HeadlightComponent;
	}

	HeadlightComponent = NewObject<UPointLightComponent>(ControlledPawn, TEXT("CameraHeadlight"));

	// Movable обязательна: свет ездит с пешкой, а статический/стационарный
	// требует запечённого освещения и просто не поедет.
	HeadlightComponent->SetMobility(EComponentMobility::Movable);

	// Затухание не обратно-квадратичное, а степенное - тогда HeadlightIntensity
	// и HeadlightRadius крутятся независимо друг от друга: радиус задаёт, докуда
	// достаёт, яркость - насколько ярко, и одно не приходится подгонять под
	// другое. Показатель 4 (а не движковый 8) - чтобы пузырь света был ровнее и
	// дальняя половина радиуса не пропадала в темноте.
	HeadlightComponent->bUseInverseSquaredFalloff = false;
	HeadlightComponent->LightFalloffExponent = 4.0f;

	// Ноль, а не движковые 20 см: источник висит ровно там, где камера, и
	// заметный радиус источника размывал бы тени и блики от самого себя.
	HeadlightComponent->SourceRadius = 0.0f;

	// Нулевое относительное смещение - лампочка ровно в начале координат пешки.
	// Камера ADefaultPawn берётся из GetActorEyesViewPoint(), т.е. может стоять
	// на BaseEyeHeight выше, но это единицы процентов радиуса света, зато
	// позицию не приходится пересчитывать каждый кадр.
	HeadlightComponent->SetupAttachment(ControlledPawn->GetRootComponent());
	HeadlightComponent->RegisterComponent();

	UE_LOG(LogTemp, Log, TEXT("EnsureHeadlight: лампочка создана на пешке %s"), *ControlledPawn->GetName());
	return HeadlightComponent;
}

void AGamePlayerController::UpdateHeadlight()
{
	UPointLightComponent* Headlight = EnsureHeadlight();
	if (!Headlight)
	{
		return;
	}

	// Настройки живут на оркестраторе (см. AAutomataOrchestrator::
	// HeadlightIntensity) и перечитываются каждый кадр - без кэша, как и всё
	// остальное, что правится в Details panel. Если оркестратора в мире нет,
	// свет остаётся с движковыми значениями: это не отказ, лампочка светит.
	if (const AAutomataOrchestrator* Orchestrator = FindOrchestrator())
	{
		Headlight->SetIntensity(Orchestrator->HeadlightIntensity);
		Headlight->SetAttenuationRadius(Orchestrator->HeadlightRadius);
		Headlight->SetLightColor(Orchestrator->HeadlightColor);
		Headlight->SetCastShadows(Orchestrator->bHeadlightCastsShadows);
	}

	Headlight->SetVisibility(true);
}

void AGamePlayerController::OnSpeedBoostStarted()
{
	ADefaultPawn* FlyingPawn = Cast<ADefaultPawn>(GetPawn());
	if (!FlyingPawn)
	{
		return;
	}

	UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(FlyingPawn->GetMovementComponent());
	if (!Movement)
	{
		return;
	}

	// Кэшируем исходную скорость только один раз - иначе повторный Started
	// без промежуточного Completed (не должно случаться штатно, но на
	// всякий случай) задавил бы исходное значение уже ускоренным.
	if (BaseFlySpeed <= 0.0f)
	{
		BaseFlySpeed = Movement->MaxSpeed;
	}

	float Multiplier = 1.0f;
	if (AAutomataOrchestrator* Orchestrator = FindOrchestrator())
	{
		Multiplier = Orchestrator->CameraSpeedMultiplier;
	}

	Movement->MaxSpeed = BaseFlySpeed * Multiplier;
}

void AGamePlayerController::OnSpeedBoostEnded()
{
	ADefaultPawn* FlyingPawn = Cast<ADefaultPawn>(GetPawn());
	if (!FlyingPawn)
	{
		return;
	}

	UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(FlyingPawn->GetMovementComponent());
	if (!Movement || BaseFlySpeed <= 0.0f)
	{
		return;
	}

	Movement->MaxSpeed = BaseFlySpeed;
}

void AGamePlayerController::OnToggleChunkedRender()
{
	// Ctrl+Z - это шаг назад (см. InputKey()), а не переключение чанкового
	// рендера. Привязка Z остаётся в Enhanced Input, поэтому нажатие с Ctrl
	// доходит и сюда - отсеиваем его здесь, как Ctrl+S у сохранения.
	if (IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl))
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleChunkedRender: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetChunkedRenderEnabled(!Orchestrator->IsChunkedRenderEnabled());
}

void AGamePlayerController::OnUndoLastAction()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnUndoLastAction: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->UndoLastAction();
}

void AGamePlayerController::OnRedoEdit()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnRedoEdit: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->RedoLastEdit();
}

void AGamePlayerController::OnCycleChunkedRenderOrder()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnCycleChunkedRenderOrder: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->CycleChunkedRenderOrder();
}

void AGamePlayerController::OnToggleWaitForChunkedRenderToFinish()
{
	// Ctrl+V - вставка из буфера (OnPasteCells()), а не эта настройка. Оба
	// обработчика висят на V и вызываются оба, так что модификатор отсеивается
	// здесь - зеркально тому, как голая Z уходит чанковому рендеру, а Ctrl+Z
	// отмене.
	if (IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl))
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleWaitForChunkedRenderToFinish: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetWaitForChunkedRenderToFinish(!Orchestrator->IsWaitingForChunkedRenderToFinish());
}

void AGamePlayerController::OnToggleCellCulling()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleCellCulling: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetCellCullingEnabled(!Orchestrator->IsCellCullingEnabled());
}

void AGamePlayerController::OnToggleRenderCullVolume()
{
	// Три действия на одной клавише, отобранные модификаторами: голая C -
	// отсечение (ниже), Ctrl+C - копирование в буфер (OnCopyCells()),
	// Ctrl+Shift+C - видимость самого куба. Enhanced Input не даёт потребовать
	// модификатор в маппинге, поэтому каждый обработчик отсеивает чужие
	// комбинации сам - иначе одно нажатие сделало бы две вещи разом, ведь на
	// клавише висят оба action'а и вызываются оба.
	//
	// Ctrl+C достался копированию, а видимость уехала под Shift сознательно:
	// Ctrl+C - общесистемная комбинация, и любая другая привязка к ней читается
	// как чужая.
	if (IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl))
	{
		if (IsShiftHeld())
		{
			OnToggleRenderCullVolumeVisibility();
		}
		// Ctrl без Shift - не наше: этим занят OnCopyCells().
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleRenderCullVolume: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetRenderCullVolumeEnabled(!Orchestrator->IsRenderCullVolumeEnabled());
}

void AGamePlayerController::OnToggleRenderCullVolumeVisibility()
{
	ARenderCullVolume* CullVolume = FindCullVolume();
	if (!CullVolume)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleRenderCullVolumeVisibility: ARenderCullVolume не найден в мире"));
		return;
	}

	const bool bNewVisible = !CullVolume->IsVolumeVisible();

	// Незавершённый драг закрываем перед скрытием - иначе куб продолжил бы
	// невидимо ездить за курсором (та же причина, что в SetSelectionModeActive()).
	if (!bNewVisible && DraggedCullVolume == CullVolume)
	{
		DraggedCullVolume->EndGizmoDrag();
		DraggedCullVolume = nullptr;
	}

	CullVolume->SetVolumeVisible(bNewVisible);
	// Ручки видны, только когда виден и сам куб, И включён режим мыши: тянуть
	// за ручки невидимую коробку означало бы менять отсечение, не видя, что
	// именно меняешь.
	CullVolume->SetGizmoVisible(bNewVisible && bSelectionModeActive);

	// Только видимость самой коробки - отсечение живёт на C и этой клавишей не
	// трогается вовсе (см. AAutomataOrchestrator::GetActiveCullVolume()).
	UE_LOG(LogTemp, Log, TEXT("Куб отсечения: %s (отсечение не затронуто)"), bNewVisible ? TEXT("показан") : TEXT("скрыт"));
}

void AGamePlayerController::OnToggleGhostShape()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleGhostShape: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetGhostShapeEnabled(!Orchestrator->IsGhostShapeEnabled());
}

bool AGamePlayerController::ShouldFireRepeat(FHotkeyRepeatState& State) const
{
	// Не GetWorld()->GetTimeSeconds(): на паузе PIE игровое время стоит, а
	// клавиши работают, и автоповтор превратился бы в "срабатывает всегда".
	const double Now = FPlatformTime::Seconds();
	const double SinceLastCall = Now - State.LastTriggeredTime;
	State.LastTriggeredTime = Now;

	// Triggered приходит каждый кадр, пока клавиша нажата, поэтому заметный
	// разрыв означает, что её отпускали: считаем нажатие новым и пропускаем
	// его без задержки.
	if (SinceLastCall > HotkeyRepeatFreshPressGap)
	{
		State.PressTime = Now;
		State.LastFireTime = Now;
		return true;
	}

	// Клавишу держат: сперва пауза перед разгоном, потом ровный автоповтор.
	if (Now - State.PressTime < HotkeyRepeatDelay || Now - State.LastFireTime < HotkeyRepeatInterval)
	{
		return false;
	}

	State.LastFireTime = Now;
	return true;
}

void AGamePlayerController::OnIncreaseSpeed()
{
	if (!ShouldFireRepeat(IncreaseSpeedRepeat))
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnIncreaseSpeed: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->AdjustSpeed(IsShiftHeld() ? SpeedAdjustStepFast : SpeedAdjustStep);
}

void AGamePlayerController::OnDecreaseSpeed()
{
	if (!ShouldFireRepeat(DecreaseSpeedRepeat))
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnDecreaseSpeed: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->AdjustSpeed(IsShiftHeld() ? -SpeedAdjustStepFast : -SpeedAdjustStep);
}

void AGamePlayerController::OnFrameAllCells()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnFrameAllCells: AAutomataOrchestrator не найден в мире"));
		return;
	}

	FrameAllCells(Orchestrator, IsShiftHeld());
}

bool AGamePlayerController::FrameAllCells(AAutomataOrchestrator* Orchestrator, bool bVisibleOnly)
{
	if (!Orchestrator)
	{
		return false;
	}

	// Явная просьба вписать в кадр - значит и масштаб ортопроекции пересчитать,
	// даже если он был накручен руками (иначе в ортопроекции Home не делал бы
	// ничего вовсе: расстояние на картинку там не влияет). Смена ракурса, в
	// отличие от этого, ручной зум сохраняет - см.
	// AGameCameraManager::HasUserOrthoWidth().
	if (AGameCameraManager* CameraManager = GetGameCameraManager())
	{
		CameraManager->ClearUserOrthoWidth();
	}

	FVector Center;
	float Radius;
	// bVisibleOnly - кадрировать по тому, что кубом/срезом/фильтром реально
	// оставлено на экране, а не по всей фигуре (см. doc-comment в заголовке -
	// без этого обрезанная кубом сетка тянула бы камеру далеко за пределы
	// самого куба, к отрезанной и невидимой части структуры).
	const bool bHaveBounds = bVisibleOnly
		? Orchestrator->ComputeVisibleCellsBounds(Center, Radius)
		: Orchestrator->ComputeAliveCellsBounds(Center, Radius);
	if (!bHaveBounds)
	{
		UE_LOG(LogTemp, Warning, TEXT("FrameAllCells: %s - кадрировать нечего"),
			bVisibleOnly ? TEXT("ничего не видно (фильтры отсекли все клетки)") : TEXT("сетка пуста"));
		return false;
	}

	if (!PlayerCameraManager)
	{
		return false;
	}

	// Ракурс не меняем - только отодвигаем/придвигаем камеру вдоль текущего
	// направления взгляда, чтобы сетка оказалась в кадре целиком.
	return FrameBounds(Center, Radius, PlayerCameraManager->GetCameraRotation().Vector(), /*bApplyRotation=*/false);
}

bool AGamePlayerController::FrameAllCellsFromDirection(AAutomataOrchestrator* Orchestrator, const FVector& ViewDirection, bool bVisibleOnly)
{
	if (!Orchestrator)
	{
		return false;
	}

	FVector Center;
	float Radius;
	const bool bHaveBounds = bVisibleOnly
		? Orchestrator->ComputeVisibleCellsBounds(Center, Radius)
		: Orchestrator->ComputeAliveCellsBounds(Center, Radius);
	if (!bHaveBounds)
	{
		UE_LOG(LogTemp, Warning, TEXT("FrameAllCellsFromDirection: %s - кадрировать нечего"),
			bVisibleOnly ? TEXT("ничего не видно (фильтры отсекли все клетки)") : TEXT("сетка пуста"));
		return false;
	}

	return FrameBounds(Center, Radius, ViewDirection, /*bApplyRotation=*/true);
}

bool AGamePlayerController::FrameBounds(const FVector& Center, float Radius, const FVector& ViewDirection, bool bApplyRotation)
{
	APawn* FlyingPawn = GetPawn();
	if (!FlyingPawn || !PlayerCameraManager)
	{
		return false;
	}

	FVector Direction = ViewDirection.GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		return false;
	}

	if (bApplyRotation)
	{
		// Питч подрезаем границами камеры-менеджера сразу: вид сверху - это
		// ровно -90°, которые он всё равно подрежет до -89.9 при первом же
		// движении мыши, и картинка бы дёрнулась.
		FRotator TargetRotation = Direction.Rotation();
		TargetRotation.Pitch = FMath::Clamp(TargetRotation.Pitch, PlayerCameraManager->ViewPitchMin, PlayerCameraManager->ViewPitchMax);
		SetControlRotation(TargetRotation);

		// Позицию считаем по УЖЕ подрезанному направлению, иначе камера стояла
		// бы строго по вертикали, а смотрела на 0.1° мимо - и центр кадра
		// уезжал бы тем сильнее, чем крупнее структура.
		Direction = TargetRotation.Vector();
	}

	// Расстояние, на котором сфера радиуса Radius целиком видна под углом
	// FOV/2: Radius / sin(FOV/2) - точное касание края кадра, плюс
	// FramingPadding, чтобы был небольшой запас по краям.
	const float HalfFovRadians = FMath::DegreesToRadians(PlayerCameraManager->GetFOVAngle()) * 0.5f;
	const float Distance = (Radius / FMath::Sin(HalfFovRadians)) * FramingPadding;
	FlyingPawn->SetActorLocation(Center - Direction * Distance);

	// В ортопроекции расстояние на видимый размер не влияет вовсе - кадрирует
	// ширина кадра, её и подгоняем. Камеру при этом всё равно отодвигаем на то
	// же расстояние, чтобы переключение проекции туда-обратно не меняло кадр.
	//
	// Кроме случая, когда масштаб выбран руками (клавишами * / /): тогда его не
	// трогаем - см. AGameCameraManager::HasUserOrthoWidth(). Снимают этот флаг
	// только явные "вписать в кадр" (FrameAllCells()/OnFrameSelection()/
	// включение ортопроекции), а смена ракурса - нет, поэтому обойти структуру
	// по осям, не потеряв подобранный зум, теперь можно.
	if (AGameCameraManager* CameraManager = GetGameCameraManager())
	{
		if (CameraManager->IsOrthographic() && !CameraManager->HasUserOrthoWidth())
		{
			CameraManager->SetOrthoWidth(ComputeOrthoWidthForRadius(Radius));
		}
	}

	UE_LOG(LogTemp, Log, TEXT("FrameBounds: камера поставлена на расстояние %.1f от центра (радиус %.1f)"), Distance, Radius);
	return true;
}

float AGamePlayerController::ComputeOrthoWidthForRadius(float Radius) const
{
	int32 ViewportX = 0;
	int32 ViewportY = 0;
	GetViewportSize(ViewportX, ViewportY);

	// Вписываем по МЕНЬШЕЙ стороне кадра: OrthoWidth задаёт ширину, высота
	// получается делением на соотношение сторон, так что на широком экране
	// диаметр надо укладывать в высоту (иначе обрежет сверху и снизу), а на
	// узком - в ширину.
	const float Aspect = (ViewportX > 0 && ViewportY > 0)
		? static_cast<float>(ViewportX) / static_cast<float>(ViewportY)
		: 1.0f;

	return 2.0f * Radius * FramingPadding * FMath::Max(1.0f, Aspect);
}

AGameCameraManager* AGamePlayerController::GetGameCameraManager() const
{
	return Cast<AGameCameraManager>(PlayerCameraManager);
}

bool AGamePlayerController::IsOrthographicCamera() const
{
	const AGameCameraManager* CameraManager = GetGameCameraManager();
	return CameraManager ? CameraManager->IsOrthographic() : false;
}

void AGamePlayerController::ShowCameraStatusMessage(const FString& Message)
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (Orchestrator)
	{
		Orchestrator->ShowStatusMessage(AAutomataOrchestrator::StatusKey_Camera, Message);
	}
}

void AGamePlayerController::OnAlignCamera(const FVector& ViewDirection, const FString& ViewName, bool bVisibleOnly)
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnAlignCamera: AAutomataOrchestrator не найден в мире"));
		return;
	}

	if (!FrameAllCellsFromDirection(Orchestrator, ViewDirection, bVisibleOnly))
	{
		ShowCameraStatusMessage(bVisibleOnly
			? FString::Printf(TEXT("Вид %s: ничего не видно (фильтры отсекли все клетки)"), *ViewName)
			: FString::Printf(TEXT("Вид %s: кадрировать нечего - сетка пуста"), *ViewName));
		return;
	}

	ShowCameraStatusMessage(bVisibleOnly
		? FString::Printf(TEXT("Вид %s (только видимое)"), *ViewName)
		: FString::Printf(TEXT("Вид %s"), *ViewName));
}

void AGamePlayerController::OnAlignCameraToOppositeSide()
{
	if (!PlayerCameraManager)
	{
		return;
	}

	// Разворот ровно текущего направления, без фиксированной оси: ракурс мог
	// быть выставлен и мышью, а не нумпадом.
	OnAlignCamera(-PlayerCameraManager->GetCameraRotation().Vector(), TEXT("с другой стороны"), IsShiftHeld());
}

void AGamePlayerController::OnToggleOrthographic()
{
	AGameCameraManager* CameraManager = GetGameCameraManager();
	if (!CameraManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleOrthographic: камера-менеджер не AGameCameraManager - ортопроекция недоступна"));
		return;
	}

	const bool bEnable = !CameraManager->IsOrthographic();
	CameraManager->SetOrthographic(bEnable);

	// Ширину сразу подгоняем под сетку - см. doc-comment обработчика: с
	// шириной по умолчанию (или оставшейся от прошлой, совсем другого размера,
	// структуры) первое включение выглядело бы как поломка.
	if (bEnable)
	{
		// Ручной зум от прошлого включения не сохраняем: структура за это время
		// могла вырасти на порядок, и подобранная тогда ширина показала бы
		// ровно ту пустоту, от которой эта подгонка и защищает.
		CameraManager->ClearUserOrthoWidth();

		AAutomataOrchestrator* Orchestrator = FindOrchestrator();
		FVector Center;
		float Radius;
		if (Orchestrator && Orchestrator->ComputeAliveCellsBounds(Center, Radius))
		{
			CameraManager->SetOrthoWidth(ComputeOrthoWidthForRadius(Radius));
		}
	}

	UE_LOG(LogTemp, Log, TEXT("OnToggleOrthographic: проекция -> %s (ширина кадра %.0f)"),
		bEnable ? TEXT("ортогональная") : TEXT("перспективная"), CameraManager->GetOrthoWidth());

	if (bEnable)
	{
		ShowCameraStatusMessage(FString::Printf(TEXT("[NumPad 5] Ортопроекция, ширина %.0f  (зум - NumPad * и /)"), CameraManager->GetOrthoWidth()));
	}
	else
	{
		ShowCameraStatusMessage(TEXT("[NumPad 5] Перспектива"));
	}
}

void AGamePlayerController::OnAdjustOrthoWidth(bool bZoomIn)
{
	AGameCameraManager* CameraManager = GetGameCameraManager();
	if (!CameraManager)
	{
		return;
	}

	CameraManager->ScaleOrthoWidth(bZoomIn ? 1.0f / OrthoZoomStep : OrthoZoomStep);

	// Сообщение показывается и при выключенной ортопроекции: значение честно
	// поменялось и подействует при её включении, а молчание выглядело бы как
	// несработавшая клавиша (та же причина, что у [ / ] при выключенном срезе).
	ShowCameraStatusMessage(FString::Printf(TEXT("[NumPad %s] Ширина ортопроекции %.0f%s"),
		bZoomIn ? TEXT("*") : TEXT("/"),
		CameraManager->GetOrthoWidth(),
		CameraManager->IsOrthographic() ? TEXT("") : TEXT("  (ортопроекция выключена - NumPad 5)")));
}

void AGamePlayerController::OnFrameSelection()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnFrameSelection: AAutomataOrchestrator не найден в мире"));
		return;
	}

	FVector Center;
	float Radius;
	if (!Orchestrator->ComputeSelectedCellsBounds(Center, Radius))
	{
		ShowCameraStatusMessage(TEXT("[NumPad .] Выделение пусто - сначала Tab, затем ЛКМ по клетке"));
		return;
	}

	// Тоже явная просьба вписать в кадр - см. FrameAllCells().
	if (AGameCameraManager* CameraManager = GetGameCameraManager())
	{
		CameraManager->ClearUserOrthoWidth();
	}

	// Ракурс сохраняем: выделение уже нашли глазами с текущей стороны,
	// разворачивать камеру незачем - надо только подъехать.
	if (PlayerCameraManager && FrameBounds(Center, Radius, PlayerCameraManager->GetCameraRotation().Vector(), /*bApplyRotation=*/false))
	{
		ShowCameraStatusMessage(TEXT("[NumPad .] Кадр по выделению"));
	}
}

bool AGamePlayerController::IsShiftHeld() const
{
	return IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);
}

void AGamePlayerController::OnMoveCullVolume(const FIntVector& CellDelta)
{
	// Вне режима выделения стрелки принадлежат пешке (полёт вперёд-назад и
	// поворот камеры), и перехватывать их там нельзя - куб ездил бы вместе с
	// полётом. Молча, без сообщения: иначе оно висело бы на экране всё время,
	// пока летаешь стрелками.
	if (!bSelectionModeActive)
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnMoveCullVolume: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->MoveCullVolumeByCells(CellDelta);
}

void AGamePlayerController::OnMoveCullVolumeUp()
{
	// С Shift вертикальная пара уходит на Z вместо Y.
	OnMoveCullVolume(IsShiftHeld() ? FIntVector(0, 0, 1) : FIntVector(0, 1, 0));
}

void AGamePlayerController::OnMoveCullVolumeDown()
{
	OnMoveCullVolume(IsShiftHeld() ? FIntVector(0, 0, -1) : FIntVector(0, -1, 0));
}

void AGamePlayerController::OnMoveCullVolumeLeft()
{
	OnMoveCullVolume(FIntVector(-1, 0, 0));
}

void AGamePlayerController::OnMoveCullVolumeRight()
{
	OnMoveCullVolume(FIntVector(1, 0, 0));
}

void AGamePlayerController::OnSetAgeFilter(int32 Age)
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnSetAgeFilter: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Возраст 9 значит не только его самого, но и всё, что старше: цифр десять,
	// а возрастов 256, и без этого хвост рампы - самая старая и обычно самая
	// крупная часть структуры - не показывался бы ни под какой клавишей. Лежит
	// он на клавише 0 (см. HotkeyRegistry: ряд сдвинут, 1 - это возраст 0), то
	// есть в конце цифрового ряда, где хвосту и место.
	//
	// Условия "а есть ли клетки старше" нет намеренно: когда их нет, ">= 9"
	// совпадает с "== 9" и ничего не меняет (см.
	// AAutomataOrchestrator::bAgeFilterIncludesOlder).
	const bool bIncludeOlder = (Age == 9);

	// Shift+цифра - добавить слой к уже показанным, а если он показан - убрать.
	// Интересны как раз сочетания: фронт роста (возраст 0) рядом со старым
	// ядром видно только вместе, по одному слою за раз их не сравнить.
	// Модификатор проверяется здесь, а не маппингом, - Enhanced Input не умеет
	// требовать его в привязке клавиши (та же идиома, что Shift+Y и Ctrl+S).
	if (IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift))
	{
		Orchestrator->ToggleAgeFilterValue(Age, bIncludeOlder);
		return;
	}

	// Та же цифра ещё раз - снять фильтр. Отдельной клавиши "показать все" нет
	// намеренно: свободной цифры не осталось - ряд занят целиком, от возраста 0
	// на клавише 1 до хвоста на клавише 0, - а какие слои сейчас активны, видно
	// из сообщения на экране. Флаг сравнивается наравне с
	// возрастом, чтобы нажатие цифры всегда приводило фильтр ровно к тому, что
	// эта цифра значит, даже если флаг перед тем правили из Details-панели.
	// GetAgeFilter() возвращает возраст только когда выбран ровно один, так
	// что после набора из нескольких слоёв цифра переключает на одиночный
	// слой, а не гасит фильтр.
	if (Orchestrator->GetAgeFilter() == Age
		&& Orchestrator->IsAgeFilterIncludingOlder() == bIncludeOlder)
	{
		Orchestrator->SetAgeFilter(-1);
		return;
	}

	Orchestrator->SetAgeFilter(Age, bIncludeOlder);
}

void AGamePlayerController::OnToggleViewSlice()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleViewSlice: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetViewSliceEnabled(!Orchestrator->IsViewSliceEnabled());
}

void AGamePlayerController::OnViewSliceNearer()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnViewSliceNearer: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Шаг задан в мировых единицах и намеренно крупный: клетка по умолчанию
	// 100 единиц, так что мельче двигать бессмысленно - срез просто не
	// пересечёт следующий слой клеток.
	if (IsShiftHeld())
	{
		Orchestrator->AdjustViewSliceThickness(-ViewSliceAdjustStep);
	}
	else
	{
		Orchestrator->AdjustViewSliceDistance(-ViewSliceAdjustStep);
	}
}

void AGamePlayerController::OnViewSliceFarther()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnViewSliceFarther: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// См. одноимённый комментарий в OnViewSliceNearer().
	if (IsShiftHeld())
	{
		Orchestrator->AdjustViewSliceThickness(ViewSliceAdjustStep);
	}
	else
	{
		Orchestrator->AdjustViewSliceDistance(ViewSliceAdjustStep);
	}
}

void AGamePlayerController::OnIncreaseStepsPerRender()
{
	// С Shift работает OnDoubleStepsPerRender() на Started - здесь молча
	// уходим, иначе за то же нажатие сработали бы оба.
	if (IsShiftHeld())
	{
		return;
	}

	if (!ShouldFireRepeat(IncreaseStepsPerRenderRepeat))
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnIncreaseStepsPerRender: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->AdjustStepsPerRender(1);
}

void AGamePlayerController::OnDecreaseStepsPerRender()
{
	// См. одноимённую проверку в OnIncreaseStepsPerRender().
	if (IsShiftHeld())
	{
		return;
	}

	if (!ShouldFireRepeat(DecreaseStepsPerRenderRepeat))
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnDecreaseStepsPerRender: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->AdjustStepsPerRender(-1);
}

void AGamePlayerController::OnDoubleStepsPerRender()
{
	if (!IsShiftHeld())
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnDoubleStepsPerRender: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->ScaleStepsPerRender(true);
}

void AGamePlayerController::OnHalveStepsPerRender()
{
	if (!IsShiftHeld())
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnHalveStepsPerRender: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->ScaleStepsPerRender(false);
}

void AGamePlayerController::OnToggleSelectionMode()
{
	// Shift+Tab - второй инструмент на той же клавише: Tab выделяет, Shift+Tab
	// рисует. Модификатор проверяется здесь, а не в привязке (Enhanced Input
	// не умеет требовать его в маппинге) - идиома Ctrl+S/Shift+F.
	if (IsShiftHeld())
	{
		SetDrawModeActive(!bDrawModeActive);
		return;
	}

	SetSelectionModeActive(!bSelectionModeActive);
}

void AGamePlayerController::SetDrawModeActive(bool bActive)
{
	if (bDrawModeActive == bActive)
	{
		return;
	}

	bDrawModeActive = bActive;

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();

	if (bActive)
	{
		// Два инструмента на одну мышь не делятся: ЛКМ в рисовании ставит
		// клетку, а в выделении тянет рамку, и одновременно они означали бы
		// разное от одного и того же нажатия.
		SetSelectionModeActive(false);
	}
	else if (Orchestrator)
	{
		Orchestrator->HideCellPreview();
		Orchestrator->HideClipboardGhost();
	}

	// Тот же единый "режим мыши", что и у выделения: камера стоит, курсор
	// виден. Ставится ПОСЛЕ SetSelectionModeActive(false) выше - иначе тот
	// вернул бы управление камерой обратно.
	SetCameraControlEnabled(!bActive);

	UE_LOG(LogTemp, Log, TEXT("Режим рисования клеток: %s"), bActive ? TEXT("включён") : TEXT("выключен"));
}

void AGamePlayerController::TickSelectionGizmo()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator || !PlayerCameraManager)
	{
		return;
	}

	// Манипулятор живёт ровно там же, где мышиное взаимодействие с клетками, -
	// в режиме выделения. В рисовании ЛКМ уже занята постановкой клеток, и
	// стрелка под курсором означала бы там третье значение одного нажатия.
	const bool bWantGizmo = bSelectionModeActive && !bDrawModeActive;
	Orchestrator->UpdateSelectionGizmo(PlayerCameraManager->GetCameraLocation(),
		PlayerCameraManager->GetFOVAngle(), bWantGizmo);

	// Пока показан манипулятор выделения, манипулятор КУБА убран: два набора
	// стрелок в одной сцене спорят за один и тот же клик, и какой схвачен,
	// становится вопросом попадания в пиксель. Куб при этом никуда не девается -
	// снял выделение, и его стрелки вернулись сами.
	//
	// Сравнение с текущим состоянием обязательно: SetGizmoVisible() трогает
	// девять компонентов и переназначает им материалы, а мы здесь каждый кадр.
	if (ARenderCullVolume* CullVolume = FindCullVolume())
	{
		const bool bWantCullGizmo = bSelectionModeActive
			&& !bDrawModeActive
			&& !Orchestrator->IsSelectionGizmoVisible()
			&& CullVolume->IsVolumeVisible();

		if (CullVolume->IsGizmoVisible() != bWantCullGizmo)
		{
			CullVolume->SetGizmoVisible(bWantCullGizmo);
		}
	}

	if (!Orchestrator->IsSelectionDragging())
	{
		return;
	}

	// Точка отсчёта - центр выделения НА МОМЕНТ ЗАХВАТА, а не текущий: центр
	// уезжает вместе с подсветкой, и драг, считающий от него, гнался бы за
	// собственным хвостом (та же причина, что у драга куба).
	float AxisParam = 0.0f;
	FVector RayOrigin = FVector::ZeroVector;
	if (ComputeGizmoAxisParam(Orchestrator->GetSelectionDragOrigin(), Orchestrator->GetSelectionDragAxis(), RayOrigin, AxisParam))
	{
		Orchestrator->UpdateSelectionDrag(AxisParam);
	}
}

void AGamePlayerController::TickCellPainting()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		return;
	}

	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ZeroVector;
	if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		// Курсор вне окна - показывать призраки негде.
		Orchestrator->HideCellPreview();
		Orchestrator->HideClipboardGhost();
		return;
	}

	// Единственное, что делает тик режима: ведёт призрак за курсором. Сами
	// клетки ставятся только по нажатию (см. OnSelectDragStarted()).
	//
	// Который из двух призраков показывать, решает ЗАЖАТЫЙ CTRL, а не
	// наполненность буфера: призрак обязан показывать то, что произойдёт по
	// текущей комбинации. Держишь Ctrl (то есть собираешься нажать Ctrl+V) -
	// видишь весь буфер там, где он ляжет; отпустил - снова одиночная клетка,
	// которую поставит ЛКМ. Иначе непустой буфер показывал бы фигуру, а клик
	// ставил бы одну клетку - призрак врал бы ровно про то, ради чего он есть.
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	if (bCtrl && Orchestrator->GetClipboardCellCount() > 0)
	{
		Orchestrator->HideCellPreview();
		Orchestrator->UpdateClipboardGhost(RayOrigin, RayDirection);
	}
	else
	{
		Orchestrator->HideClipboardGhost();
		Orchestrator->UpdateCellPreview(RayOrigin, RayDirection);
	}
}

void AGamePlayerController::OnCopyCells()
{
	// Ctrl+C, как везде. Голая C - отсечение, Ctrl+Shift+C - видимость куба
	// (см. OnToggleRenderCullVolume(), который зеркально отсеивает наш случай),
	// поэтому Shift здесь означает "нажали не нас".
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	if (!bCtrl || IsShiftHeld())
	{
		return;
	}

	if (AAutomataOrchestrator* Orchestrator = FindOrchestrator())
	{
		Orchestrator->CopyCellsToClipboard();
	}
}

void AGamePlayerController::OnPasteCells()
{
	// Ctrl+V; голая V - ожидание чанкового рендера (тот обработчик отсеивает
	// Ctrl сам).
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	if (!bCtrl)
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		return;
	}

	// Вставка целится курсором, значит курсор должен быть - включаем режим
	// рисования, если он выключен. Иначе первое же Ctrl+V в полёте вставляло бы
	// вслепую туда, куда смотрит центр экрана.
	if (!bDrawModeActive)
	{
		SetDrawModeActive(true);
	}

	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ZeroVector;
	if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		return;
	}

	Orchestrator->PasteClipboard(RayOrigin, RayDirection);
}

void AGamePlayerController::RotateClipboard(int32 Axis, bool bClockwise)
{
	// Только в режиме рисования: вне его стрелки принадлежат кубу отсечения, а
	// PageUp/PageDown не должны молча крутить невидимый буфер.
	if (!bDrawModeActive)
	{
		return;
	}

	if (AAutomataOrchestrator* Orchestrator = FindOrchestrator())
	{
		Orchestrator->RotateClipboard(Axis, bClockwise);
	}
}

void AGamePlayerController::OnRotateClipboardYawLeft()
{
	RotateClipboard(/*Axis=*/2, /*bClockwise=*/false);
}

void AGamePlayerController::OnRotateClipboardYawRight()
{
	RotateClipboard(/*Axis=*/2, /*bClockwise=*/true);
}

void AGamePlayerController::OnRotateClipboardPitchUp()
{
	RotateClipboard(/*Axis=*/0, /*bClockwise=*/true);
}

void AGamePlayerController::OnRotateClipboardPitchDown()
{
	RotateClipboard(/*Axis=*/0, /*bClockwise=*/false);
}

void AGamePlayerController::OnRotateClipboardRollLeft()
{
	RotateClipboard(/*Axis=*/1, /*bClockwise=*/false);
}

void AGamePlayerController::OnRotateClipboardRollRight()
{
	RotateClipboard(/*Axis=*/1, /*bClockwise=*/true);
}

void AGamePlayerController::OnEraseCellPressed()
{
	if (!bDrawModeActive)
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		return;
	}

	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ZeroVector;
	if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		return;
	}

	Orchestrator->PaintCellUnderCursor(RayOrigin, RayDirection, /*bErase=*/true);
}

void AGamePlayerController::SetSelectionModeActive(bool bActive)
{
	if (bSelectionModeActive == bActive)
	{
		return;
	}

	bSelectionModeActive = bActive;
	// Выход из режима не должен оставлять "подвисшую" рамку драга, если
	// пользователь вышел из режима прямо во время удержания ЛКМ.
	bIsDraggingSelection = false;

	// Выделение выход из режима ПЕРЕЖИВАЕТ. Снимать его здесь было пробовано и
	// отвергнуто пользователем: выход бывает мимолётным (любая клавиша полёта
	// выводит из режима, см. InputKey()), и терять из-за этого набранное
	// мышкой выделение хуже, чем изредка получить Delete или Ctrl+D по тому,
	// что выделено давно.
	//
	// А вот незакрытый ДРАГ за гизмо закрыть обязаны: подсветка во время него
	// сдвинута на показанное перемещение, и выход из режима, оставив её так,
	// нарисовал бы клетки там, где их нет. Закрытие идёт штатным путём, то есть
	// перенос доводится до сетки и журнала, а не выбрасывается.
	if (!bActive)
	{
		if (AAutomataOrchestrator* Orchestrator = FindOrchestrator())
		{
			Orchestrator->EndSelectionDrag();
		}
	}

	// Манипулятор куба живёт ровно столько же, сколько режим взаимодействия
	// мышью: вне его нет курсора, чтобы за ручку взяться. Незавершённый драг
	// закрываем, иначе куб остался бы "приклеен" к курсору после Tab.
	if (ARenderCullVolume* CullVolume = FindCullVolume())
	{
		if (!bActive && DraggedCullVolume)
		{
			DraggedCullVolume->EndGizmoDrag();
			DraggedCullVolume = nullptr;
		}
		// И режим мыши, и видимость куба - см. OnToggleRenderCullVolumeVisibility():
		// Tab не должен вытаскивать ручки у спрятанной (Ctrl+Shift+C) коробки.
		CullVolume->SetGizmoVisible(bActive && CullVolume->IsVolumeVisible());
	}

	// Единый режим взаимодействия мышкой - и клетки, и HUD (см. doc-comment
	// в заголовке).
	SetCameraControlEnabled(!bActive);

	UE_LOG(LogTemp, Log, TEXT("Режим выделения клеток/HUD: %s"), bActive ? TEXT("включён") : TEXT("выключен"));
}

ARenderCullVolume* AGamePlayerController::FindCullVolume()
{
	if (!IsValid(CachedCullVolume))
	{
		CachedCullVolume = Cast<ARenderCullVolume>(UGameplayStatics::GetActorOfClass(GetWorld(), ARenderCullVolume::StaticClass()));
	}
	return CachedCullVolume;
}

AAutomataOrchestrator* AGamePlayerController::FindOrchestrator()
{
	if (!IsValid(CachedOrchestrator))
	{
		CachedOrchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	}
	return CachedOrchestrator;
}

bool AGamePlayerController::ComputeGizmoAxisParam(const FVector& AxisOrigin, const FVector& Axis, FVector& OutRayOrigin, float& OutAxisParam) const
{
	FVector RayDirection = FVector::ZeroVector;
	if (!const_cast<AGamePlayerController*>(this)->DeprojectMousePositionToWorld(OutRayOrigin, RayDirection))
	{
		return false;
	}

	// Ближайшая точка на прямой оси к прямой луча - классическая задача о двух
	// скрещивающихся прямых. Обе направляющие единичные, поэтому a = c = 1 и
	// знаменатель вырождается в (1 - b^2): он же и есть мера параллельности.
	const FVector AxisDir = Axis.GetSafeNormal();
	const FVector RayDir = RayDirection.GetSafeNormal();
	const FVector ToAxis = AxisOrigin - OutRayOrigin;

	const float B = FVector::DotProduct(AxisDir, RayDir);
	const float Denominator = 1.0f - B * B;
	if (FMath::Abs(Denominator) < KINDA_SMALL_NUMBER)
	{
		// Смотрим почти вдоль оси - ближайшая точка убегает в бесконечность, и
		// драг превратился бы в рывок на много тысяч юнитов.
		return false;
	}

	const float D = FVector::DotProduct(AxisDir, ToAxis);
	const float E = FVector::DotProduct(RayDir, ToAxis);
	OutAxisParam = (B * E - D) / Denominator;
	return true;
}

void AGamePlayerController::RebindPawnVerticalMovement()
{
	ADefaultPawn* FlyingPawn = Cast<ADefaultPawn>(GetPawn());
	if (!FlyingPawn || !FlyingPawn->InputComponent)
	{
		// Пешки или её InputComponent'а ещё нет - попробуем на следующем кадре.
		return;
	}

	if (VerticalMovementBoundPawn == FlyingPawn)
	{
		return;
	}
	VerticalMovementBoundPawn = FlyingPawn;

	UInputComponent* PawnInput = FlyingPawn->InputComponent;

	// Снимаем движковый биндинг целиком: точечно убрать из него LeftControl и C
	// нельзя, сами клавиши лежат в приватном статическом реестре UPlayerInput.
	//
	// Заодно снимаем и горизонтальные оси - по другой причине, но с тем же
	// исходом. Половина хоткеев проекта это Ctrl+буква, и буквы те же, что у
	// полёта: Ctrl+D (тираж) уезжал камерой вправо, Ctrl+S (сохранение) - назад.
	// Клавиша доходит и до хоткея, и до оси - Enhanced Input и оси движения
	// работают параллельно, - поэтому единственное место, где это разруливается,
	// сами оси (см. PawnMoveForward()).
	static const FName EngineMoveUpAxis(TEXT("DefaultPawn_MoveUp"));
	static const FName EngineMoveForwardAxis(TEXT("DefaultPawn_MoveForward"));
	static const FName EngineMoveRightAxis(TEXT("DefaultPawn_MoveRight"));
	const int32 RemovedCount = PawnInput->AxisBindings.RemoveAll(
		[](const FInputAxisBinding& Binding)
		{
			return Binding.AxisName == EngineMoveUpAxis
				|| Binding.AxisName == EngineMoveForwardAxis
				|| Binding.AxisName == EngineMoveRightAxis;
		});

	// Своя ось с тем же поведением, но без конфликтующих клавиш. Имя своё, а не
	// движковое: движковые привязки клавиш к DefaultPawn_MoveUp никуда не
	// делись, и переиспользование имени вернуло бы Ctrl и C обратно.
	//
	// Пробела здесь тоже нет: он отдан паузе (см. InputKey()). Подъём остался на
	// E, спуск на Q - пробел был третьей клавишей на то же действие, тогда как
	// пауза без него осталась бы на P, куда рука не тянется.
	static const FName OwnMoveUpAxis(TEXT("CellularAutomata_MoveUp"));
	static const FName OwnMoveForwardAxis(TEXT("CellularAutomata_MoveForward"));
	static const FName OwnMoveRightAxis(TEXT("CellularAutomata_MoveRight"));
	if (PlayerInput)
	{
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveUpAxis, EKeys::E, 1.0f));
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveUpAxis, EKeys::Q, -1.0f));

		// Клавиши горизонтали повторяют движковый набор один в один, включая
		// стрелки: смысл перевешивания не в том, чтобы поменять раскладку, а
		// только в том, чтобы оси проходили через свой обработчик и умели
		// замолкать под Ctrl.
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveForwardAxis, EKeys::W, 1.0f));
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveForwardAxis, EKeys::S, -1.0f));
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveForwardAxis, EKeys::Up, 1.0f));
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveForwardAxis, EKeys::Down, -1.0f));
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveRightAxis, EKeys::D, 1.0f));
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveRightAxis, EKeys::A, -1.0f));
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveRightAxis, EKeys::Right, 1.0f));
		PlayerInput->AddAxisMapping(FInputAxisKeyMapping(OwnMoveRightAxis, EKeys::Left, -1.0f));
	}

	// Все три оси - через СВОИ обработчики на контроллере, а не напрямую в
	// ADefaultPawn: только так у них появляется место, где проверить Ctrl.
	PawnInput->BindAxis(OwnMoveUpAxis, this, &AGamePlayerController::PawnMoveUp);
	PawnInput->BindAxis(OwnMoveForwardAxis, this, &AGamePlayerController::PawnMoveForward);
	PawnInput->BindAxis(OwnMoveRightAxis, this, &AGamePlayerController::PawnMoveRight);

	UE_LOG(LogTemp, Log, TEXT("RebindPawnVerticalMovement: снято движковых биндингов осей полёта: %d; вертикаль на E/Q (Ctrl и C освобождены под хоткеи, пробел - под паузу), горизонталь молчит под Ctrl"),
		RemovedCount);
}

bool AGamePlayerController::IsCtrlHeld() const
{
	return IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
}

void AGamePlayerController::PawnMoveForward(float Value)
{
	// Ctrl зажат - значит человек набирает хоткей (Ctrl+S, Ctrl+D, Ctrl+C...),
	// а не летит. Клавиша при этом всё равно доходит и сюда, и до хоткея, так
	// что промолчать может только ось.
	if (Value == 0.0f || IsCtrlHeld())
	{
		return;
	}

	if (ADefaultPawn* FlyingPawn = Cast<ADefaultPawn>(GetPawn()))
	{
		FlyingPawn->MoveForward(Value);
	}
}

void AGamePlayerController::PawnMoveRight(float Value)
{
	if (Value == 0.0f || IsCtrlHeld())
	{
		return;
	}

	if (ADefaultPawn* FlyingPawn = Cast<ADefaultPawn>(GetPawn()))
	{
		FlyingPawn->MoveRight(Value);
	}
}

void AGamePlayerController::PawnMoveUp(float Value)
{
	// Вертикали Ctrl не мешает (он снят с неё ещё в
	// RebindPawnVerticalMovement()), но правило держим одно на все три оси:
	// иначе Ctrl+E и Ctrl+Q оставались бы единственными комбинациями, которые
	// двигают камеру, и разбираться, почему именно они, пришлось бы заново.
	if (Value == 0.0f || IsCtrlHeld())
	{
		return;
	}

	if (ADefaultPawn* FlyingPawn = Cast<ADefaultPawn>(GetPawn()))
	{
		FlyingPawn->MoveUp_World(Value);
	}
}

void AGamePlayerController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Разовая (на пешку) пересборка - до раннего выхода ниже, иначе она бы
	// не случилась вовсе, пока в мире нет куба отсечения.
	RebindPawnVerticalMovement();

	// Тоже до раннего выхода и по той же причине. Пока лампочка выключена -
	// ровно одна проверка флага; пока горит - перечитывание её настроек, чтобы
	// они правились на живую (см. UpdateHeadlight()), и заодно самолечение,
	// если пешка сменилась или её не было в момент нажатия.
	if (bHeadlightEnabled)
	{
		UpdateHeadlight();
	}

	// Рисование - тоже до раннего выхода: призрак под курсором обязан жить и
	// тогда, когда куба отсечения в мире нет вовсе.
	if (bDrawModeActive)
	{
		TickCellPainting();
	}

	// И манипулятор выделения - по той же причине: он к кубу отсечения
	// отношения не имеет, а ранний выход ниже стоит именно на кубе.
	TickSelectionGizmo();

	ARenderCullVolume* CullVolume = FindCullVolume();
	if (!CullVolume || !CullVolume->IsGizmoVisible())
	{
		return;
	}

	if (PlayerCameraManager)
	{
		CullVolume->UpdateGizmoScreenSize(PlayerCameraManager->GetCameraLocation(), PlayerCameraManager->GetFOVAngle());
	}

	if (DraggedCullVolume == CullVolume && CullVolume->IsGizmoDragging())
	{
		FVector RayOrigin = FVector::ZeroVector;
		float AxisParam = 0.0f;
		// Начало оси - зафиксированное на момент захвата, НЕ текущая позиция
		// актёра: иначе драг сам себя догоняет и куб дрожит между двумя
		// точками (см. doc-comment GetGizmoDragOrigin()).
		if (ComputeGizmoAxisParam(CullVolume->GetGizmoDragOrigin(), CullVolume->GetActiveGizmoAxis(), RayOrigin, AxisParam))
		{
			// Shift и Ctrl читаются каждый кадр драга, а не запоминаются на
			// его начало (в отличие от модификатора выделения рамкой): здесь
			// это не "режим операции", а непрерывная подстройка - зажал
			// посреди драга и пропорции (Shift) или ускорение (Ctrl)
			// подхватились, отпустил и снова тянется одна ось один к одному.
			const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
			CullVolume->UpdateGizmoDrag(AxisParam, IsShiftHeld(), bCtrl);
		}
	}
}

void AGamePlayerController::OnSelectDragStarted()
{
	// В режиме рисования ЛКМ ставит одну клетку, а не тянет рамку - проверяется
	// первым, режимы взаимоисключающи (см. SetDrawModeActive()).
	if (bDrawModeActive)
	{
		AAutomataOrchestrator* Orchestrator = FindOrchestrator();
		FVector RayOrigin = FVector::ZeroVector;
		FVector RayDirection = FVector::ZeroVector;
		if (Orchestrator && DeprojectMousePositionToWorld(RayOrigin, RayDirection))
		{
			// Зажатый Ctrl при непустом буфере - ВСТАВКА, и это ровно то
			// состояние, в котором под курсором уже висит призрак буфера (см.
			// TickCellPainting()). Клик по тому, что видишь, - и есть вставка;
			// отдельный Ctrl+V для этого оказался неинтуитивным именно потому,
			// что призрак уже стоял на месте и просил нажатия мышью.
			const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
			if (bCtrl && Orchestrator->GetClipboardCellCount() > 0)
			{
				Orchestrator->PasteClipboard(RayOrigin, RayDirection);
			}
			else
			{
				Orchestrator->PaintCellUnderCursor(RayOrigin, RayDirection, /*bErase=*/false);
			}
		}
		return;
	}

	if (!bSelectionModeActive)
	{
		return;
	}

	// Манипулятор ВЫДЕЛЕНИЯ - первым из двух: он появляется только когда
	// выделение есть, стоит прямо на нём, и человек, целясь в его стрелку,
	// заведомо не собирался тянуть рамку. Куб отсечения проверяется следом.
	if (AAutomataOrchestrator* Orchestrator = FindOrchestrator())
	{
		FVector RayOrigin = FVector::ZeroVector;
		FVector RayDirection = FVector::ZeroVector;
		if (DeprojectMousePositionToWorld(RayOrigin, RayDirection))
		{
			FVector HandleAxis = FVector::ZeroVector;
			const int32 Axis = Orchestrator->TraceSelectionGizmo(RayOrigin, RayDirection, HandleAxis);

			FVector SelectionCenter = FVector::ZeroVector;
			float SelectionRadius = 0.0f;
			if (Axis != INDEX_NONE && Orchestrator->ComputeSelectedCellsBounds(SelectionCenter, SelectionRadius))
			{
				// Параметр оси считается от центра выделения - той же точки, от
				// которой его будет считать каждый кадр драга (см.
				// GetSelectionDragOrigin()); разные точки в начале и в
				// продолжении дали бы рывок на первом же движении мыши.
				float AxisParam = 0.0f;
				FVector UnusedOrigin = FVector::ZeroVector;
				if (ComputeGizmoAxisParam(SelectionCenter, HandleAxis, UnusedOrigin, AxisParam))
				{
					Orchestrator->BeginSelectionDrag(Axis, HandleAxis, AxisParam);
					return;
				}
			}
		}
	}

	// Манипулятор куба проверяется ПЕРЕД рамкой: если нажали на ручку, ЛКМ занята
	// перетаскиванием куба, и выделение в этот раз не начинается вовсе.
	if (ARenderCullVolume* CullVolume = FindCullVolume())
	{
		FVector RayOrigin = FVector::ZeroVector;
		FVector RayDirection = FVector::ZeroVector;
		if (CullVolume->IsGizmoVisible() && DeprojectMousePositionToWorld(RayOrigin, RayDirection))
		{
			FVector HandleAxis = FVector::ZeroVector;
			const EVolumeGizmoHandle Handle = CullVolume->TraceGizmoHandle(RayOrigin, RayDirection, HandleAxis);
			if (Handle != EVolumeGizmoHandle::None)
			{
				float AxisParam = 0.0f;
				FVector UnusedOrigin = FVector::ZeroVector;
				if (ComputeGizmoAxisParam(CullVolume->GetActorLocation(), HandleAxis, UnusedOrigin, AxisParam))
				{
					CullVolume->BeginGizmoDrag(Handle, HandleAxis, AxisParam);
					DraggedCullVolume = CullVolume;
					return;
				}
			}
		}
	}

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	if (GetMousePosition(MouseX, MouseY))
	{
		DragStartScreenPos = FVector2D(MouseX, MouseY);
		bIsDraggingSelection = true;

		// Модификатор снимается в момент старта драга, не отпускания (как в
		// большинстве редакторов) - Ctrl приоритетнее Shift, если зажаты оба.
		if (IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl))
		{
			PendingSelectionCombineMode = ESelectionCombineMode::Subtract;
		}
		else if (IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift))
		{
			PendingSelectionCombineMode = ESelectionCombineMode::Add;
		}
		else
		{
			PendingSelectionCombineMode = ESelectionCombineMode::Replace;
		}
	}
}

void AGamePlayerController::OnSelectDragFinished()
{
	// В режиме рисования отпускание кнопки не значит ничего: клетка уже
	// поставлена нажатием, и рамки, которую надо было бы закрыть, здесь нет.
	if (bDrawModeActive)
	{
		return;
	}

	// Тянули выделение за стрелку - переносим клетки одной правкой и выходим:
	// рамки в этот раз не было.
	if (AAutomataOrchestrator* Orchestrator = FindOrchestrator())
	{
		if (Orchestrator->IsSelectionDragging())
		{
			Orchestrator->EndSelectionDrag();
			return;
		}
	}

	// Тянули ручку манипулятора, а не рамку - завершаем драг (там же уйдёт
	// запрос на перерисовку) и выходим, выделение здесь ни при чём.
	if (DraggedCullVolume)
	{
		DraggedCullVolume->EndGizmoDrag();
		DraggedCullVolume = nullptr;
		return;
	}

	if (!bIsDraggingSelection)
	{
		return;
	}
	bIsDraggingSelection = false;

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	if (!GetMousePosition(MouseX, MouseY))
	{
		return;
	}

	const FVector2D CurrentScreenPos(MouseX, MouseY);
	const FVector2D RectMin(FMath::Min(DragStartScreenPos.X, CurrentScreenPos.X), FMath::Min(DragStartScreenPos.Y, CurrentScreenPos.Y));
	const FVector2D RectMax(FMath::Max(DragStartScreenPos.X, CurrentScreenPos.X), FMath::Max(DragStartScreenPos.Y, CurrentScreenPos.Y));

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnSelectDragFinished: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Мышь между нажатием и отпусканием почти не сдвинулась - это клик, а
	// не драг: выбираем одиночную клетку под курсором (луч через решётку,
	// см. AAutomataOrchestrator::SelectCellUnderCursor()), а не пустой
	// прямоугольник нулевой площади, в который не попал бы ни один центр
	// клетки. Модификаторы (Shift/Ctrl) работают те же - режим уже снят в
	// OnSelectDragStarted().
	if (FVector2D::Distance(DragStartScreenPos, CurrentScreenPos) <= ClickDragThresholdPixels)
	{
		FVector RayOrigin = FVector::ZeroVector;
		FVector RayDirection = FVector::ZeroVector;
		if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection))
		{
			UE_LOG(LogTemp, Warning, TEXT("OnSelectDragFinished: не удалось депроецировать курсор в мировой луч"));
			return;
		}

		// Ghost Shape заменил детальный рендер - клеток на экране нет вовсе,
		// и выбирать их бессмысленно: клик по кубику силуэта означает "поставь
		// сюда куб отсечения". Новой клавиши и нового режима не нужно, потому
		// что в этом состоянии у клика просто нет другого разумного смысла.
		// Это закрывает разрыв в сценарии осмотра: H показывает всё целиком,
		// клик выбирает область, дальше C и срез.
		if (Orchestrator->ShouldGhostShapeReplaceDetailedRender()
			&& Orchestrator->MoveCullVolumeToChunkUnderCursor(RayOrigin, RayDirection))
		{
			return;
		}

		Orchestrator->SelectCellUnderCursor(RayOrigin, RayDirection, PendingSelectionCombineMode);
		return;
	}

	ULocalPlayer* LP = GetLocalPlayer();
	if (!LP || !LP->ViewportClient || !LP->ViewportClient->Viewport)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnSelectDragFinished: нет доступного viewport для проекции"));
		return;
	}

	// Матрица вида-проекции считается один раз на всю операцию выделения
	// (не на клетку) - тот же API, что использует UGameplayStatics::
	// ProjectWorldToScreen внутри, но без повторных накладных расходов на
	// каждый вызов при потенциально миллионах живых клеток.
	FSceneViewProjectionData ProjectionData;
	if (!LP->GetProjectionData(LP->ViewportClient->Viewport, ProjectionData))
	{
		UE_LOG(LogTemp, Warning, TEXT("OnSelectDragFinished: не удалось получить ProjectionData"));
		return;
	}

	const FMatrix ViewProjectionMatrix = ProjectionData.ComputeViewProjectionMatrix();
	const FVector2D ViewportSize(ProjectionData.GetConstrainedViewRect().Width(), ProjectionData.GetConstrainedViewRect().Height());

	Orchestrator->SelectCellsInScreenRect(ViewProjectionMatrix, ViewportSize, RectMin, RectMax, PendingSelectionCombineMode);
}

void AGamePlayerController::OnExtractSelection()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnExtractSelection: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->StartFromSelection();
}

void AGamePlayerController::OnInvertSelection()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnInvertSelection: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->InvertSelection();
}

void AGamePlayerController::OnBakeCellsToMesh()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnBakeCellsToMesh: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->BakeCellsToMesh();
}

void AGamePlayerController::OnDeleteSelectedCells()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnDeleteSelectedCells: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->DeleteSelectedCells();
}

void AGamePlayerController::OnMoveCullVolumeToSelection()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnMoveCullVolumeToSelection: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->MoveCullVolumeToSelection();
}

void AGamePlayerController::OnSelectCellsInCullVolume()
{
	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnSelectCellsInCullVolume: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Модификатор снимается в момент нажатия - та же идиома, что и у
	// OnSelectDragStarted() (Ctrl приоритетнее Shift, если зажаты оба).
	ESelectionCombineMode CombineMode = ESelectionCombineMode::Replace;
	if (IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl))
	{
		CombineMode = ESelectionCombineMode::Subtract;
	}
	else if (IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift))
	{
		CombineMode = ESelectionCombineMode::Add;
	}

	Orchestrator->SelectCellsInCullVolume(CombineMode);
}

void AGamePlayerController::OnSaveOrSaveAs()
{
	// Действие на S замаппено без модификатора (см. SetupInputComponent()) -
	// голый S должен молча уйти камере (DefaultPawn), а не сработать как
	// сохранение.
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	if (!bCtrl)
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnSaveOrSaveAs: AAutomataOrchestrator не найден в мире"));
		return;
	}

	const bool bShift = IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);
	if (bShift)
	{
		Orchestrator->SaveStateAs(); // Ctrl+Shift+S
	}
	else
	{
		Orchestrator->SaveState(); // Ctrl+S
	}
}

void AGamePlayerController::OnArrayCells()
{
	// Как и у S/O - маппинг самой клавиши без модификатора, Ctrl проверяется
	// здесь; голая D продолжает двигать камеру вправо (DefaultPawn).
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	if (!bCtrl)
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnArrayCells: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->ArrayCells();
}

void AGamePlayerController::OnLoadState()
{
	// Как и у S - маппинг сам ключа без модификатора, Ctrl проверяется здесь.
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	if (!bCtrl)
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = FindOrchestrator();
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnLoadState: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->LoadStateFromFile();
}

void AGamePlayerController::SetCameraControlEnabled(bool bEnable)
{
	bCanLookAround = bEnable;
	if (bEnable)
	{
		RestoreGameInputMode();
	} else
	{
		DisableGameInputMode();
	}
}

void AGamePlayerController::DisableGameInputMode()
{   
	// Настраиваем режим GameAndUI - игра продолжает работать, но мышь видна и UI получает ввод
	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputMode.SetHideCursorDuringCapture(false);
	
	SetIgnoreMoveInput(true);
	SetIgnoreLookInput(true);
	
	SetInputMode(InputMode);
	bShowMouseCursor = true;
    
	// Отключаем все события мыши в игре
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
	bEnableTouchEvents = true;
    
	// Убираем все привязки осей из InputComponent
	if (InputComponent)
	{
		// Полностью очищаем все привязки осей
		InputComponent->AxisBindings.Empty();
        
		UE_LOG(LogTemp, Warning, TEXT("All input bindings cleared"));
	}
	
	// Отключаем ввод на Pawn
	if (APawn* ControlledPawn = GetPawn())
	{
		ControlledPawn->DisableInput(this);
		UE_LOG(LogTemp, Warning, TEXT("Pawn input disabled"));
	}
    
	UE_LOG(LogTemp, Warning, TEXT("SetWebBrowserInputMode completed"));
}

void AGamePlayerController::RestoreGameInputMode()
{
	UE_LOG(LogTemp, Warning, TEXT("Restoring game input mode"));
    
	// Включаем ввод
	SetIgnoreMoveInput(false);
	SetIgnoreLookInput(false);
    
	// Восстанавливаем input mode
	FInputModeGameOnly InputMode;
	SetInputMode(InputMode);
	bShowMouseCursor = false;
    
	// Включаем события
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
    
	// Включаем ввод на Pawn
	if (APawn* ControlledPawn = GetPawn())
	{
		ControlledPawn->EnableInput(this);
	}
    
	UE_LOG(LogTemp, Warning, TEXT("Game input mode restored"));
}

void AGamePlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (PlayerCameraManager)
	{
		float Fov = 60.0f;
		PlayerCameraManager->SetFOV(Fov);
		UE_LOG(LogTemp, Log, TEXT("Camera FOV set to %f degrees"), Fov);
	}

	// Игра стартует с обычным освещённым рендером - Unlit больше не
	// форсируется автоматически, переключается вручную хоткеями 1 (Lit) /
	// 2 (Unlit, экономит на освещении при большом числе инстансированных
	// клеток автомата).
}
