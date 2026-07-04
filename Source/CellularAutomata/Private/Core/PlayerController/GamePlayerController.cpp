// Fill out your copyright notice in the Description page of Project Settings.

#include "Core/PlayerController/GamePlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Orchestration/AutomataOrchestrator.h"

// YourPlayerController.cpp
void AGamePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	ToggleSimulationAction = NewObject<UInputAction>(this, TEXT("IA_ToggleSimulation"));
	ToggleSimulationAction->ValueType = EInputActionValueType::Boolean;

	StepOnceAction = NewObject<UInputAction>(this, TEXT("IA_StepOnce"));
	StepOnceAction->ValueType = EInputActionValueType::Boolean;

	ResetSimulationAction = NewObject<UInputAction>(this, TEXT("IA_ResetSimulation"));
	ResetSimulationAction->ValueType = EInputActionValueType::Boolean;

	SetLitModeAction = NewObject<UInputAction>(this, TEXT("IA_SetLitMode"));
	SetLitModeAction->ValueType = EInputActionValueType::Boolean;

	SetUnlitModeAction = NewObject<UInputAction>(this, TEXT("IA_SetUnlitMode"));
	SetUnlitModeAction->ValueType = EInputActionValueType::Boolean;

	SimulationMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Simulation"));
	SimulationMappingContext->MapKey(ToggleSimulationAction, EKeys::SpaceBar);
	SimulationMappingContext->MapKey(StepOnceAction, EKeys::F);
	SimulationMappingContext->MapKey(ResetSimulationAction, EKeys::R);
	SimulationMappingContext->MapKey(SetLitModeAction, EKeys::One);
	SimulationMappingContext->MapKey(SetUnlitModeAction, EKeys::Two);

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		Subsystem->AddMappingContext(SimulationMappingContext, 0);
	}

	if (UEnhancedInputComponent* EnhancedInputComp = Cast<UEnhancedInputComponent>(InputComponent))
	{
		EnhancedInputComp->BindAction(ToggleSimulationAction, ETriggerEvent::Started, this, &AGamePlayerController::OnToggleSimulation);
		EnhancedInputComp->BindAction(StepOnceAction, ETriggerEvent::Started, this, &AGamePlayerController::OnStepOnce);
		EnhancedInputComp->BindAction(ResetSimulationAction, ETriggerEvent::Started, this, &AGamePlayerController::OnResetSimulation);
		EnhancedInputComp->BindAction(SetLitModeAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSetLitMode);
		EnhancedInputComp->BindAction(SetUnlitModeAction, ETriggerEvent::Started, this, &AGamePlayerController::OnSetUnlitMode);
	}
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

void AGamePlayerController::OnStepOnce()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnStepOnce: AAutomataOrchestrator не найден в мире"));
		return;
	}

	if (Orchestrator->IsSimulationRunning())
	{
		UE_LOG(LogTemp, Warning, TEXT("OnStepOnce: непрерывная симуляция уже идёт (Space) - ручной шаг F игнорируется"));
		return;
	}

	Orchestrator->Next();
}

void AGamePlayerController::OnResetSimulation()
{
	AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	if (!Orchestrator)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnResetSimulation: AAutomataOrchestrator не найден в мире"));
		return;
	}

	Orchestrator->GenerateRandom();
}

void AGamePlayerController::OnSetLitMode()
{
	ConsoleCommand(TEXT("VIEWMODE LIT"));
}

void AGamePlayerController::OnSetUnlitMode()
{
	ConsoleCommand(TEXT("VIEWMODE UNLIT"));
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