// Fill out your copyright notice in the Description page of Project Settings.

#include "Core/PlayerController/GamePlayerController.h"
#include "Camera/PlayerCameraManager.h"

// YourPlayerController.cpp
void AGamePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
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

	// Unlit-режим - как в редакторе, чтобы не считать освещение при
	// большом количестве инстансированных клеток автомата
	ConsoleCommand(TEXT("VIEWMODE UNLIT"));
}