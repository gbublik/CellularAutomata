// Fill out your copyright notice in the Description page of Project Settings.

#include "Core/PlayerController/GamePlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Orchestration/AutomataOrchestrator.h"
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

	ResetSimulationAction = NewObject<UInputAction>(this, TEXT("IA_ResetSimulation"));
	ResetSimulationAction->ValueType = EInputActionValueType::Boolean;

	SetLitModeAction = NewObject<UInputAction>(this, TEXT("IA_SetLitMode"));
	SetLitModeAction->ValueType = EInputActionValueType::Boolean;

	SetUnlitModeAction = NewObject<UInputAction>(this, TEXT("IA_SetUnlitMode"));
	SetUnlitModeAction->ValueType = EInputActionValueType::Boolean;

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

	IncreaseSpeedAction = NewObject<UInputAction>(this, TEXT("IA_IncreaseSpeed"));
	IncreaseSpeedAction->ValueType = EInputActionValueType::Boolean;

	DecreaseSpeedAction = NewObject<UInputAction>(this, TEXT("IA_DecreaseSpeed"));
	DecreaseSpeedAction->ValueType = EInputActionValueType::Boolean;

	FrameAllCellsAction = NewObject<UInputAction>(this, TEXT("IA_FrameAllCells"));
	FrameAllCellsAction->ValueType = EInputActionValueType::Boolean;

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

	// P (пауза) намеренно не маппится сюда - см. InputKey() ниже, она
	// перехватывается на уровне сырых оконных событий в обход Enhanced Input.
	SimulationMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Simulation"));
	SimulationMappingContext->MapKey(FastStepAction, EKeys::F);
	SimulationMappingContext->MapKey(ResetSimulationAction, EKeys::R);
	SimulationMappingContext->MapKey(SetLitModeAction, EKeys::One);
	SimulationMappingContext->MapKey(SetUnlitModeAction, EKeys::Two);
	SimulationMappingContext->MapKey(SpeedBoostAction, EKeys::LeftShift);
	SimulationMappingContext->MapKey(ToggleChunkedRenderAction, EKeys::Z);
	SimulationMappingContext->MapKey(CycleChunkedRenderOrderAction, EKeys::X);
	SimulationMappingContext->MapKey(ToggleWaitForChunkedRenderToFinishAction, EKeys::V);
	SimulationMappingContext->MapKey(ToggleCellCullingAction, EKeys::B);
	// Основной ряд (=/-) и NumPad (+/-) - чтобы работало независимо от того,
	// есть ли у клавиатуры цифровой блок.
	SimulationMappingContext->MapKey(IncreaseSpeedAction, EKeys::Equals);
	SimulationMappingContext->MapKey(IncreaseSpeedAction, EKeys::Add);
	SimulationMappingContext->MapKey(DecreaseSpeedAction, EKeys::Hyphen);
	SimulationMappingContext->MapKey(DecreaseSpeedAction, EKeys::Subtract);
	SimulationMappingContext->MapKey(FrameAllCellsAction, EKeys::Home);
	SimulationMappingContext->MapKey(IncreaseStepsPerRenderAction, EKeys::T);
	SimulationMappingContext->MapKey(DecreaseStepsPerRenderAction, EKeys::G);
	SimulationMappingContext->MapKey(ToggleSelectionModeAction, EKeys::Tab);
	SimulationMappingContext->MapKey(SelectDragAction, EKeys::LeftMouseButton);
	SimulationMappingContext->MapKey(ExtractSelectionAction, EKeys::Enter);
	SimulationMappingContext->MapKey(InvertSelectionAction, EKeys::I);
	SimulationMappingContext->MapKey(BakeCellsToMeshAction, EKeys::M);

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
		EnhancedInputComp->BindAction(ResetSimulationAction, ETriggerEvent::Started, this, &AGamePlayerController::OnResetSimulation);
		EnhancedInputComp->BindAction(SetLitModeAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSetLitMode);
		EnhancedInputComp->BindAction(SetUnlitModeAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSetUnlitMode);
		EnhancedInputComp->BindAction(SpeedBoostAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSpeedBoostStarted);
		EnhancedInputComp->BindAction(SpeedBoostAction, ETriggerEvent::Completed, this, &AGamePlayerController::OnSpeedBoostEnded);
		EnhancedInputComp->BindAction(ToggleChunkedRenderAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleChunkedRender);
		EnhancedInputComp->BindAction(CycleChunkedRenderOrderAction, ETriggerEvent::Started, this, &AGamePlayerController::OnCycleChunkedRenderOrder);
		EnhancedInputComp->BindAction(ToggleWaitForChunkedRenderToFinishAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleWaitForChunkedRenderToFinish);
		EnhancedInputComp->BindAction(ToggleCellCullingAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleCellCulling);
		// Triggered - держа +/-, Speed продолжает меняться каждый кадр, а не
		// только на однократное нажатие (аналогично F/OnStepOnce()).
		EnhancedInputComp->BindAction(IncreaseSpeedAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnIncreaseSpeed);
		EnhancedInputComp->BindAction(DecreaseSpeedAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnDecreaseSpeed);
		EnhancedInputComp->BindAction(FrameAllCellsAction, ETriggerEvent::Started, this, &AGamePlayerController::OnFrameAllCells);
		// Triggered - держа T/G, StepsPerRender продолжает меняться каждый
		// кадр, а не только на однократное нажатие (аналогично +/- для Speed).
		EnhancedInputComp->BindAction(IncreaseStepsPerRenderAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnIncreaseStepsPerRender);
		EnhancedInputComp->BindAction(DecreaseStepsPerRenderAction, ETriggerEvent::Triggered, this, &AGamePlayerController::OnDecreaseStepsPerRender);
		EnhancedInputComp->BindAction(ToggleSelectionModeAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleSelectionMode);
		EnhancedInputComp->BindAction(SelectDragAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSelectDragStarted);
		EnhancedInputComp->BindAction(SelectDragAction, ETriggerEvent::Completed, this, &AGamePlayerController::OnSelectDragFinished);
		EnhancedInputComp->BindAction(ExtractSelectionAction, ETriggerEvent::Started, this, &AGamePlayerController::OnExtractSelection);
		EnhancedInputComp->BindAction(InvertSelectionAction, ETriggerEvent::Started, this, &AGamePlayerController::OnInvertSelection);
		EnhancedInputComp->BindAction(BakeCellsToMeshAction, ETriggerEvent::Started, this, &AGamePlayerController::OnBakeCellsToMesh);
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

void AGamePlayerController::OnSetLitMode()
{
	ConsoleCommand(TEXT("VIEWMODE LIT"));
}

void AGamePlayerController::OnSetUnlitMode()
{
	ConsoleCommand(TEXT("VIEWMODE UNLIT"));
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

	FVector Center;
	float Radius;
	if (!Orchestrator->ComputeAliveCellsBounds(Center, Radius))
	{
		UE_LOG(LogTemp, Warning, TEXT("OnFrameAllCells: сетка пуста - кадрировать нечего"));
		return;
	}

	APawn* FlyingPawn = GetPawn();
	if (!FlyingPawn || !PlayerCameraManager)
	{
		return;
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

	UE_LOG(LogTemp, Log, TEXT("OnFrameAllCells: камера поставлена на расстояние %.1f от центра сетки (радиус %.1f)"), Distance, Radius);
}

void AGamePlayerController::OnIncreaseStepsPerRender()
{
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
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnDecreaseStepsPerRender: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->AdjustStepsPerRender(-1);
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

	SetCameraControlEnabled(!bActive);

	UE_LOG(LogTemp, Log, TEXT("Режим выделения клеток: %s"), bActive ? TEXT("включён") : TEXT("выключен"));
}

void AGamePlayerController::OnSelectDragStarted()
{
	if (!bSelectionModeActive)
	{
		return;
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