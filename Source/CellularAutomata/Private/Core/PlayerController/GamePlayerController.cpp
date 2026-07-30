// Fill out your copyright notice in the Description page of Project Settings.

#include "Core/PlayerController/GamePlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Orchestration/AutomataOrchestrator.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "Engine/LocalPlayer.h"
#include "SceneView.h"

// YourPlayerController.cpp
void AGamePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	FastStepAction = NewObject<UInputAction>(this, TEXT("IA_FastStep"));
	FastStepAction->ValueType = EInputActionValueType::Boolean;

	// По действию на профиль рендера. Их ровно столько, сколько клавиш F1-F4
	// ниже; если в таблицу RenderPresets::GetAll() добавится пятый профиль,
	// ему нужно будет добавить и действие, и клавишу, и биндинг - поэтому
	// количество здесь одно на всё, а не переоткрывается в трёх местах.
	constexpr int32 NumRenderPresetHotkeys = 4;
	RenderPresetActions.Reset(NumRenderPresetHotkeys);
	for (int32 PresetIndex = 0; PresetIndex < NumRenderPresetHotkeys; ++PresetIndex)
	{
		UInputAction* PresetAction = NewObject<UInputAction>(this, *FString::Printf(TEXT("IA_RenderPreset%d"), PresetIndex));
		PresetAction->ValueType = EInputActionValueType::Boolean;
		RenderPresetActions.Add(PresetAction);
	}

	ToggleBackgroundAction = NewObject<UInputAction>(this, TEXT("IA_ToggleBackground"));
	ToggleBackgroundAction->ValueType = EInputActionValueType::Boolean;

	SpeedBoostAction = NewObject<UInputAction>(this, TEXT("IA_SpeedBoost"));
	SpeedBoostAction->ValueType = EInputActionValueType::Boolean;

	ToggleChunkedRenderAction = NewObject<UInputAction>(this, TEXT("IA_ToggleChunkedRender"));
	ToggleChunkedRenderAction->ValueType = EInputActionValueType::Boolean;

	CycleChunkedRenderOrderAction = NewObject<UInputAction>(this, TEXT("IA_CycleChunkedRenderOrder"));
	CycleChunkedRenderOrderAction->ValueType = EInputActionValueType::Boolean;

	ToggleWaitForChunkedRenderToFinishAction = NewObject<UInputAction>(this, TEXT("IA_ToggleWaitForChunkedRenderToFinish"));
	ToggleWaitForChunkedRenderToFinishAction->ValueType = EInputActionValueType::Boolean;

	ToggleCellCullingAction = NewObject<UInputAction>(this, TEXT("IA_ToggleCellCulling"));
	ToggleCellCullingAction->ValueType = EInputActionValueType::Boolean;

	ToggleRenderCullVolumeAction = NewObject<UInputAction>(this, TEXT("IA_ToggleRenderCullVolume"));
	ToggleRenderCullVolumeAction->ValueType = EInputActionValueType::Boolean;

	ToggleGhostShapeAction = NewObject<UInputAction>(this, TEXT("IA_ToggleGhostShape"));
	ToggleGhostShapeAction->ValueType = EInputActionValueType::Boolean;

	IncreaseSpeedAction = NewObject<UInputAction>(this, TEXT("IA_IncreaseSpeed"));
	IncreaseSpeedAction->ValueType = EInputActionValueType::Boolean;

	DecreaseSpeedAction = NewObject<UInputAction>(this, TEXT("IA_DecreaseSpeed"));
	DecreaseSpeedAction->ValueType = EInputActionValueType::Boolean;

	FrameAllCellsAction = NewObject<UInputAction>(this, TEXT("IA_FrameAllCells"));
	FrameAllCellsAction->ValueType = EInputActionValueType::Boolean;

	MoveCullVolumeUpAction = NewObject<UInputAction>(this, TEXT("IA_MoveCullVolumeUp"));
	MoveCullVolumeUpAction->ValueType = EInputActionValueType::Boolean;

	MoveCullVolumeDownAction = NewObject<UInputAction>(this, TEXT("IA_MoveCullVolumeDown"));
	MoveCullVolumeDownAction->ValueType = EInputActionValueType::Boolean;

	MoveCullVolumeLeftAction = NewObject<UInputAction>(this, TEXT("IA_MoveCullVolumeLeft"));
	MoveCullVolumeLeftAction->ValueType = EInputActionValueType::Boolean;

	MoveCullVolumeRightAction = NewObject<UInputAction>(this, TEXT("IA_MoveCullVolumeRight"));
	MoveCullVolumeRightAction->ValueType = EInputActionValueType::Boolean;

	ToggleViewSliceAction = NewObject<UInputAction>(this, TEXT("IA_ToggleViewSlice"));
	ToggleViewSliceAction->ValueType = EInputActionValueType::Boolean;

	ViewSliceNearerAction = NewObject<UInputAction>(this, TEXT("IA_ViewSliceNearer"));
	ViewSliceNearerAction->ValueType = EInputActionValueType::Boolean;

	ViewSliceFartherAction = NewObject<UInputAction>(this, TEXT("IA_ViewSliceFarther"));
	ViewSliceFartherAction->ValueType = EInputActionValueType::Boolean;

	IncreaseStepsPerRenderAction = NewObject<UInputAction>(this, TEXT("IA_IncreaseStepsPerRender"));
	IncreaseStepsPerRenderAction->ValueType = EInputActionValueType::Boolean;

	DecreaseStepsPerRenderAction = NewObject<UInputAction>(this, TEXT("IA_DecreaseStepsPerRender"));
	DecreaseStepsPerRenderAction->ValueType = EInputActionValueType::Boolean;

	ToggleSelectionModeAction = NewObject<UInputAction>(this, TEXT("IA_ToggleSelectionMode"));
	ToggleSelectionModeAction->ValueType = EInputActionValueType::Boolean;

	SelectDragAction = NewObject<UInputAction>(this, TEXT("IA_SelectDrag"));
	SelectDragAction->ValueType = EInputActionValueType::Boolean;

	ExtractSelectionAction = NewObject<UInputAction>(this, TEXT("IA_ExtractSelection"));
	ExtractSelectionAction->ValueType = EInputActionValueType::Boolean;

	InvertSelectionAction = NewObject<UInputAction>(this, TEXT("IA_InvertSelection"));
	InvertSelectionAction->ValueType = EInputActionValueType::Boolean;

	BakeCellsToMeshAction = NewObject<UInputAction>(this, TEXT("IA_BakeCellsToMesh"));
	BakeCellsToMeshAction->ValueType = EInputActionValueType::Boolean;

	DeleteSelectedCellsAction = NewObject<UInputAction>(this, TEXT("IA_DeleteSelectedCells"));
	DeleteSelectedCellsAction->ValueType = EInputActionValueType::Boolean;

	MoveCullVolumeToSelectionAction = NewObject<UInputAction>(this, TEXT("IA_MoveCullVolumeToSelection"));
	MoveCullVolumeToSelectionAction->ValueType = EInputActionValueType::Boolean;

	SelectCellsInCullVolumeAction = NewObject<UInputAction>(this, TEXT("IA_SelectCellsInCullVolume"));
	SelectCellsInCullVolumeAction->ValueType = EInputActionValueType::Boolean;

	SaveStateAction = NewObject<UInputAction>(this, TEXT("IA_SaveState"));
	SaveStateAction->ValueType = EInputActionValueType::Boolean;

	LoadStateAction = NewObject<UInputAction>(this, TEXT("IA_LoadState"));
	LoadStateAction->ValueType = EInputActionValueType::Boolean;

	// P (пауза), R (сброс) и N (новый сид) намеренно не маппятся сюда - см.
	// InputKey() ниже, все три перехватываются на уровне сырых оконных событий
	// в обход Enhanced Input (у R и N тот же лаговый баг пропущенного нажатия,
	// что был у P - см. doc-comment InputKey()).
	SimulationMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Simulation"));
	SimulationMappingContext->MapKey(FastStepAction, EKeys::F);
	// F1-F4, а не 1-4: цифровой ряд отдан фильтру по возрасту (см. InputKey()),
	// а возрастные слои перебирают постоянно при осмотре, тогда как профиль
	// рендера ставят изредка.
	//
	// Отдельная клавиша на профиль, а не одна циклическая: профилей четыре, и
	// "сделать быстро" нужно немедленно, а не после трёх нажатий вслепую -
	// перебор имеет смысл там, где вариантов много и они равноправны
	// (ChunkedRenderOrder на X), а не там, где есть явные "как задумано" и
	// "максимально быстро" на краях списка.
	static const FKey RenderPresetKeys[] = { EKeys::F1, EKeys::F2, EKeys::F3, EKeys::F4 };
	static_assert(UE_ARRAY_COUNT(RenderPresetKeys) == 4, "Клавиш профилей рендера должно быть столько же, сколько действий выше");
	for (int32 PresetIndex = 0; PresetIndex < RenderPresetActions.Num(); ++PresetIndex)
	{
		SimulationMappingContext->MapKey(RenderPresetActions[PresetIndex], RenderPresetKeys[PresetIndex]);
	}
	SimulationMappingContext->MapKey(ToggleBackgroundAction, EKeys::U);
	SimulationMappingContext->MapKey(SpeedBoostAction, EKeys::LeftShift);
	SimulationMappingContext->MapKey(ToggleChunkedRenderAction, EKeys::Z);
	SimulationMappingContext->MapKey(CycleChunkedRenderOrderAction, EKeys::X);
	SimulationMappingContext->MapKey(ToggleWaitForChunkedRenderToFinishAction, EKeys::V);
	SimulationMappingContext->MapKey(ToggleCellCullingAction, EKeys::B);
	SimulationMappingContext->MapKey(ToggleRenderCullVolumeAction, EKeys::C);
	SimulationMappingContext->MapKey(ToggleGhostShapeAction, EKeys::H);
	// Основной ряд (=/-) и NumPad (+/-) - чтобы работало независимо от того,
	// есть ли у клавиатуры цифровой блок.
	SimulationMappingContext->MapKey(IncreaseSpeedAction, EKeys::Equals);
	SimulationMappingContext->MapKey(IncreaseSpeedAction, EKeys::Add);
	SimulationMappingContext->MapKey(DecreaseSpeedAction, EKeys::Hyphen);
	SimulationMappingContext->MapKey(DecreaseSpeedAction, EKeys::Subtract);
	SimulationMappingContext->MapKey(FrameAllCellsAction, EKeys::Home);
	SimulationMappingContext->MapKey(IncreaseStepsPerRenderAction, EKeys::T);
	SimulationMappingContext->MapKey(DecreaseStepsPerRenderAction, EKeys::G);
	// [ и ] освободились, когда StepsPerRender переехал на T/G.
	// Стрелки заняты ADefaultPawn (полёт и поворот), поэтому обработчики ниже
	// работают только в режиме выделения, где ввод пешки отключён - см.
	// OnMoveCullVolume().
	SimulationMappingContext->MapKey(MoveCullVolumeUpAction, EKeys::Up);
	SimulationMappingContext->MapKey(MoveCullVolumeDownAction, EKeys::Down);
	SimulationMappingContext->MapKey(MoveCullVolumeLeftAction, EKeys::Left);
	SimulationMappingContext->MapKey(MoveCullVolumeRightAction, EKeys::Right);
	SimulationMappingContext->MapKey(ToggleViewSliceAction, EKeys::J);
	SimulationMappingContext->MapKey(ViewSliceNearerAction, EKeys::LeftBracket);
	SimulationMappingContext->MapKey(ViewSliceFartherAction, EKeys::RightBracket);
	SimulationMappingContext->MapKey(ToggleSelectionModeAction, EKeys::Tab);
	SimulationMappingContext->MapKey(SelectDragAction, EKeys::LeftMouseButton);
	SimulationMappingContext->MapKey(ExtractSelectionAction, EKeys::Enter);
	SimulationMappingContext->MapKey(InvertSelectionAction, EKeys::I);
	SimulationMappingContext->MapKey(BakeCellsToMeshAction, EKeys::M);
	SimulationMappingContext->MapKey(DeleteSelectedCellsAction, EKeys::Delete);
	SimulationMappingContext->MapKey(MoveCullVolumeToSelectionAction, EKeys::K);
	SimulationMappingContext->MapKey(SelectCellsInCullVolumeAction, EKeys::L);
	// S/O замапплены БЕЗ модификатора - Enhanced Input не даёт потребовать
	// Ctrl прямо в маппинге ключа (в отличие от старых FInputChord).
	// Ctrl(+Shift) проверяется внутри OnSaveOrSaveAs()/OnLoadState() - та же
	// идиома, что у Ctrl/Shift в OnSelectDragStarted(). Голый S по-прежнему
	// уходит камере (DefaultPawn, движение назад) - это осознанный
	// побочный эффект удержания Ctrl+S во время полёта, см. doc-comment.
	SimulationMappingContext->MapKey(SaveStateAction, EKeys::S);
	SimulationMappingContext->MapKey(LoadStateAction, EKeys::O);

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		Subsystem->AddMappingContext(SimulationMappingContext, 0);
	}

	if (UEnhancedInputComponent* EnhancedInputComp = Cast<UEnhancedInputComponent>(InputComponent))
	{
		// Started/Completed (не Triggered) - F теперь тумблер (обычное
		// нажатие) или hold-режим (Shift+F), а не "срабатывает каждый кадр,
		// пока зажата", как раньше.
		EnhancedInputComp->BindAction(FastStepAction, ETriggerEvent::Started, this, &AGamePlayerController::OnFastStepPressed);
		EnhancedInputComp->BindAction(FastStepAction, ETriggerEvent::Completed, this, &AGamePlayerController::OnFastStepReleased);
		// Started - профиль применяется однократно на нажатие; удержание F1
		// не должно переприменять его каждый кадр (полный RenderGridImmediate()
		// на кадр), поэтому не Triggered.
		//
		// Обработчики именованные, а не лямбды с захватом индекса: BindAction
		// принимает указатель на метод, и это ровно та же схема, что у
		// четырёх стрелок движения куба отсечения.
		void (AGamePlayerController::* RenderPresetHandlers[])() = {
			&AGamePlayerController::OnApplyRenderPreset0,
			&AGamePlayerController::OnApplyRenderPreset1,
			&AGamePlayerController::OnApplyRenderPreset2,
			&AGamePlayerController::OnApplyRenderPreset3
		};
		for (int32 PresetIndex = 0; PresetIndex < RenderPresetActions.Num(); ++PresetIndex)
		{
			EnhancedInputComp->BindAction(RenderPresetActions[PresetIndex], ETriggerEvent::Started, this, RenderPresetHandlers[PresetIndex]);
		}
		EnhancedInputComp->BindAction(ToggleBackgroundAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleBackground);
		EnhancedInputComp->BindAction(SpeedBoostAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSpeedBoostStarted);
		EnhancedInputComp->BindAction(SpeedBoostAction, ETriggerEvent::Completed, this, &AGamePlayerController::OnSpeedBoostEnded);
		EnhancedInputComp->BindAction(ToggleChunkedRenderAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleChunkedRender);
		EnhancedInputComp->BindAction(CycleChunkedRenderOrderAction, ETriggerEvent::Started, this, &AGamePlayerController::OnCycleChunkedRenderOrder);
		EnhancedInputComp->BindAction(ToggleWaitForChunkedRenderToFinishAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleWaitForChunkedRenderToFinish);
		EnhancedInputComp->BindAction(ToggleCellCullingAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleCellCulling);
		EnhancedInputComp->BindAction(ToggleRenderCullVolumeAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleRenderCullVolume);
		EnhancedInputComp->BindAction(ToggleGhostShapeAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleGhostShape);
		// Triggered - держа +/-, Speed продолжает меняться каждый кадр, а не
		// только на однократное нажатие (аналогично F/OnStepOnce()).
		EnhancedInputComp->BindAction(IncreaseSpeedAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnIncreaseSpeed);
		EnhancedInputComp->BindAction(DecreaseSpeedAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnDecreaseSpeed);
		EnhancedInputComp->BindAction(FrameAllCellsAction, ETriggerEvent::Started, this, &AGamePlayerController::OnFrameAllCells);
		// Triggered - держа T/G, StepsPerRender продолжает меняться каждый
		// кадр, а не только на однократное нажатие (аналогично +/- для Speed).
		EnhancedInputComp->BindAction(IncreaseStepsPerRenderAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnIncreaseStepsPerRender);
		EnhancedInputComp->BindAction(DecreaseStepsPerRenderAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnDecreaseStepsPerRender);
		// Те же клавиши ещё раз, но на Started и под Shift - переход к
		// следующей/предыдущей степени двойки. Started, а не Triggered:
		// удвоение на каждом кадре удержания улетело бы в потолок мгновенно.
		// Какой из двух обработчиков сработает, решает проверка Shift внутри
		// них самих - Enhanced Input не умеет требовать модификатор в
		// маппинге клавиши.
		EnhancedInputComp->BindAction(IncreaseStepsPerRenderAction, ETriggerEvent::Started, this, &AGamePlayerController::OnDoubleStepsPerRender);
		EnhancedInputComp->BindAction(DecreaseStepsPerRenderAction, ETriggerEvent::Started, this, &AGamePlayerController::OnHalveStepsPerRender);
		// Triggered - удержание повторяет сдвиг, как у +/- для Speed.
		EnhancedInputComp->BindAction(MoveCullVolumeUpAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnMoveCullVolumeUp);
		EnhancedInputComp->BindAction(MoveCullVolumeDownAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnMoveCullVolumeDown);
		EnhancedInputComp->BindAction(MoveCullVolumeLeftAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnMoveCullVolumeLeft);
		EnhancedInputComp->BindAction(MoveCullVolumeRightAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnMoveCullVolumeRight);
		EnhancedInputComp->BindAction(ToggleViewSliceAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleViewSlice);
		// Triggered - срез подбирают на глаз, непрерывно, а не однократным
		// нажатием (как +/- для Speed).
		EnhancedInputComp->BindAction(ViewSliceNearerAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnViewSliceNearer);
		EnhancedInputComp->BindAction(ViewSliceFartherAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnViewSliceFarther);
		EnhancedInputComp->BindAction(ToggleSelectionModeAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleSelectionMode);
		EnhancedInputComp->BindAction(SelectDragAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSelectDragStarted);
		EnhancedInputComp->BindAction(SelectDragAction, ETriggerEvent::Completed, this, &AGamePlayerController::OnSelectDragFinished);
		EnhancedInputComp->BindAction(ExtractSelectionAction, ETriggerEvent::Started, this, &AGamePlayerController::OnExtractSelection);
		EnhancedInputComp->BindAction(InvertSelectionAction, ETriggerEvent::Started, this, &AGamePlayerController::OnInvertSelection);
		EnhancedInputComp->BindAction(BakeCellsToMeshAction, ETriggerEvent::Started, this, &AGamePlayerController::OnBakeCellsToMesh);
		EnhancedInputComp->BindAction(DeleteSelectedCellsAction, ETriggerEvent::Started, this, &AGamePlayerController::OnDeleteSelectedCells);
		EnhancedInputComp->BindAction(MoveCullVolumeToSelectionAction, ETriggerEvent::Started, this, &AGamePlayerController::OnMoveCullVolumeToSelection);
		EnhancedInputComp->BindAction(SelectCellsInCullVolumeAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSelectCellsInCullVolume);
		EnhancedInputComp->BindAction(SaveStateAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSaveOrSaveAs);
		EnhancedInputComp->BindAction(LoadStateAction, ETriggerEvent::Started, this, &AGamePlayerController::OnLoadState);
	}
}

bool AGamePlayerController::InputKey(const FInputKeyEventArgs& Params)
{
	// Enhanced Input оценивает свои триггеры (Started/Triggered/...) раз за
	// кадр, по текущему состоянию клавиши "нажата сейчас или нет" - если
	// игровой поток лагает (тяжёлый AddInstances/перестройка HISM-дерева при
	// большом числе клеток), один кадр может растянуться настолько, что
	// короткое нажатие+отпускание P целиком уместится между двумя такими
	// выборками и не будет замечено вообще - пауза "не срабатывает", и чем
	// сильнее лаг, тем чаще. InputKey() же вызывается немедленно на каждое
	// оконное сообщение (WM_KEYDOWN/WM_KEYUP), независимо от длины кадра, до
	// периодической выборки Enhanced Input - поэтому пауза обрабатывается
	// здесь напрямую, в обход маппинга. IE_Pressed (не IE_Repeat) - как и
	// раньше, срабатывает один раз на нажатие, не повторяется при удержании.
	if (Params.Key == EKeys::P && Params.Event == IE_Pressed)
	{
		OnToggleSimulation();
	}

	// R (сброс, OnResetSimulation()) - тот же самый баг и то же решение, что
	// у P выше: раньше был замаплен через Enhanced Input и мог пропускать
	// короткие нажатия под тяжёлым лагом (пользователь сообщил "не всегда
	// срабатывает" - именно этот симптом). Перенесён сюда, в обход маппинга.
	if (Params.Key == EKeys::R && Params.Event == IE_Pressed)
	{
		OnResetSimulation();
	}

	// N (новый сид, OnNewSeed()) - третий случай того же бага: реролл нажимают
	// именно тогда, когда картинка не нравится и сетка уже разрослась, т.е.
	// ровно в момент худшего лага, когда выборка Enhanced Input раз в кадр
	// пропускает короткие нажатия. Второй, независимый источник того же
	// симптома - молчаливый отказ GenerateRandom() во время фонового шага -
	// закрыт отложенным путём на стороне оркестратора (см. bNewSeedPending).
	if (Params.Key == EKeys::N && Params.Event == IE_Pressed)
	{
		OnNewSeed();
	}

	// Цифры 0-9 - фильтр по возрасту (9 - ещё и всё, что старше). Здесь, а не через Enhanced
	// Input, по причине, не связанной с лагом: десять клавиш одного вида - это
	// десять UInputAction, десять MapKey и десять обработчиков ради одного
	// switch. Читаемость дороже единообразия, а задержки эти клавиши не
	// критичны, в отличие от P/R/N выше.
	if (Params.Event == IE_Pressed)
	{
		static const FKey DigitKeys[10] = {
			EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four,
			EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine
		};

		for (int32 Digit = 0; Digit < 10; ++Digit)
		{
			if (Params.Key == DigitKeys[Digit])
			{
				OnSetAgeFilter(Digit);
				break;
			}
		}
	}

	return Super::InputKey(Params);
}

void AGamePlayerController::OnToggleSimulation()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnFastStepPressed: AAutomataOrchestrator не найден в мире"));
		return;
	}

	if (Orchestrator->IsSimulationRunning())
	{
		UE_LOG(LogTemp, Warning, TEXT("OnFastStepPressed: непрерывная симуляция уже идёт (P) - F игнорируется"));
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
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (Orchestrator && Orchestrator->IsFastStepActive())
	{
		Orchestrator->StopFastStep();
	}
}

void AGamePlayerController::OnResetSimulation()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnResetSimulation: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->ResetToInitialState();
}

void AGamePlayerController::OnNewSeed()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnNewSeed: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->NewSeed();
}

void AGamePlayerController::OnApplyRenderPreset(int32 PresetIndex)
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleBackground: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetBackgroundVisible(!Orchestrator->IsBackgroundVisible());
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
	if (AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass())))
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
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleChunkedRender: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetChunkedRenderEnabled(!Orchestrator->IsChunkedRenderEnabled());
}

void AGamePlayerController::OnCycleChunkedRenderOrder()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnCycleChunkedRenderOrder: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->CycleChunkedRenderOrder();
}

void AGamePlayerController::OnToggleWaitForChunkedRenderToFinish()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleWaitForChunkedRenderToFinish: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetWaitForChunkedRenderToFinish(!Orchestrator->IsWaitingForChunkedRenderToFinish());
}

void AGamePlayerController::OnToggleCellCulling()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleCellCulling: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetCellCullingEnabled(!Orchestrator->IsCellCullingEnabled());
}

void AGamePlayerController::OnToggleRenderCullVolume()
{
	// Ctrl+C - не отсечение, а видимость самого куба. Enhanced Input не даёт
	// потребовать Ctrl прямо в маппинге ключа, поэтому модификатор проверяется
	// здесь - та же идиома, что у Ctrl+S/Ctrl+Shift+S в OnSaveOrSaveAs().
	if (IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl))
	{
		OnToggleRenderCullVolumeVisibility();
		return;
	}

	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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

	// Спрятанный куб не режет (см. AAutomataOrchestrator::GetActiveCullVolume()),
	// так что Ctrl+C заодно приостанавливает/возвращает отсечение - но
	// bEnableRenderCullVolume при этом не перезаписывается, поэтому показ
	// возвращает ровно то состояние, в котором его оставил C.
	UE_LOG(LogTemp, Log, TEXT("Куб отсечения: %s (пока скрыт, клетки не отсекаются)"), bNewVisible ? TEXT("показан") : TEXT("скрыт"));
}

void AGamePlayerController::OnToggleGhostShape()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleGhostShape: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetGhostShapeEnabled(!Orchestrator->IsGhostShapeEnabled());
}

void AGamePlayerController::OnIncreaseSpeed()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnIncreaseSpeed: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->AdjustSpeed(SpeedAdjustStep);
}

void AGamePlayerController::OnDecreaseSpeed()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnDecreaseSpeed: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->AdjustSpeed(-SpeedAdjustStep);
}

void AGamePlayerController::OnFrameAllCells()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnFrameAllCells: AAutomataOrchestrator не найден в мире"));
		return;
	}

	FrameAllCells(Orchestrator);
}

bool AGamePlayerController::FrameAllCells(AAutomataOrchestrator* Orchestrator)
{
	if (!Orchestrator)
	{
		return false;
	}

	FVector Center;
	float Radius;
	if (!Orchestrator->ComputeAliveCellsBounds(Center, Radius))
	{
		UE_LOG(LogTemp, Warning, TEXT("FrameAllCells: сетка пуста - кадрировать нечего"));
		return false;
	}

	APawn* FlyingPawn = GetPawn();
	if (!FlyingPawn || !PlayerCameraManager)
	{
		return false;
	}

	// Расстояние, на котором сфера радиуса Radius целиком видна под углом
	// FOV/2: Radius / sin(FOV/2) - точное касание края кадра, плюс
	// FramingPadding, чтобы был небольшой запас по краям.
	const float HalfFovRadians = FMath::DegreesToRadians(PlayerCameraManager->GetFOVAngle()) * 0.5f;
	const float Distance = (Radius / FMath::Sin(HalfFovRadians)) * FramingPadding;

	// Ракурс не меняем - только отодвигаем/придвигаем камеру вдоль текущего
	// направления взгляда, чтобы сетка оказалась в кадре целиком.
	const FVector ViewDirection = PlayerCameraManager->GetCameraRotation().Vector();
	FlyingPawn->SetActorLocation(Center - ViewDirection * Distance);

	UE_LOG(LogTemp, Log, TEXT("FrameAllCells: камера поставлена на расстояние %.1f от центра сетки (радиус %.1f)"), Distance, Radius);
	return true;
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

	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnSetAgeFilter: AAutomataOrchestrator не найден в мире"));
		return;
	}

	// Последняя цифра значит не только возраст 9, но и всё, что старше: цифр
	// десять, а возрастов 256, и без этого хвост рампы - самая старая и обычно
	// самая крупная часть структуры - не показывался бы ни под какой цифрой.
	// Условия "а есть ли клетки старше" нет намеренно: когда их нет, ">= 9"
	// совпадает с "== 9" и ничего не меняет (см.
	// AAutomataOrchestrator::bAgeFilterIncludesOlder).
	const bool bIncludeOlder = (Age == 9);

	// Та же цифра ещё раз - снять фильтр. Отдельной клавиши "показать все" нет
	// намеренно: ноль отдан НУЛЕВОМУ возрасту (только что родившиеся клетки,
	// первый цвет рампы и самый интересный слой), а какая цифра сейчас
	// активна, видно из сообщения на экране. Флаг сравнивается наравне с
	// возрастом, чтобы нажатие цифры всегда приводило фильтр ровно к тому, что
	// эта цифра значит, даже если флаг перед тем правили из Details-панели.
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
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnToggleViewSlice: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->SetViewSliceEnabled(!Orchestrator->IsViewSliceEnabled());
}

void AGamePlayerController::OnViewSliceNearer()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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

	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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

	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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

	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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

	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnHalveStepsPerRender: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->ScaleStepsPerRender(false);
}

void AGamePlayerController::OnToggleSelectionMode()
{
	SetSelectionModeActive(!bSelectionModeActive);
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
		// Tab не должен вытаскивать ручки у спрятанной (Ctrl+C) коробки.
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

void AGamePlayerController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

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
			// Shift читается каждый кадр драга, а не запоминается на его
			// начало (в отличие от модификатора выделения рамкой): здесь это
			// не "режим операции", а непрерывная подстройка - зажал посреди
			// драга и пропорции подхватились, отпустил и снова тянется одна
			// ось.
			CullVolume->UpdateGizmoDrag(AxisParam, IsShiftHeld());
		}
	}
}

void AGamePlayerController::OnSelectDragStarted()
{
	if (!bSelectionModeActive)
	{
		return;
	}

	// Манипулятор проверяется ПЕРЕД рамкой: если нажали на ручку, ЛКМ занята
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

	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnExtractSelection: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->StartFromSelection();
}

void AGamePlayerController::OnInvertSelection()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnInvertSelection: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->InvertSelection();
}

void AGamePlayerController::OnBakeCellsToMesh()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnBakeCellsToMesh: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->BakeCellsToMesh();
}

void AGamePlayerController::OnDeleteSelectedCells()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnDeleteSelectedCells: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->DeleteSelectedCells();
}

void AGamePlayerController::OnMoveCullVolumeToSelection()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnMoveCullVolumeToSelection: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->MoveCullVolumeToSelection();
}

void AGamePlayerController::OnSelectCellsInCullVolume()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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

	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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

void AGamePlayerController::OnLoadState()
{
	// Как и у S - маппинг сам ключа без модификатора, Ctrl проверяется здесь.
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	if (!bCtrl)
	{
		return;
	}

	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
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