// Fill out your copyright notice in the Description page of Project Settings.

#include "Orchestration/GamePlayerController.h"

#include "WebBrowser.h"

// YourPlayerController.cpp
void AGamePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
    
	if (InputComponent)
	{
		InputComponent->BindAxis("Turn", this, &AGamePlayerController::Turn);
		InputComponent->BindAxis("LookUp", this, &AGamePlayerController::LookUp);
	}
}

void AGamePlayerController::Turn(float Value)
{
	if (bCanLookAround && Value != 0.0f)
	{
		AddYawInput(Value);
	}
}

void AGamePlayerController::LookUp(float Value)
{
	if (bCanLookAround && Value != 0.0f)
	{
		AddPitchInput(Value);
	}
}

void AGamePlayerController::SetCameraControlEnabled(bool bEnable)
{
	bCanLookAround = bEnable;
}

void AGamePlayerController::SetUIInputMode(UUserWidget* WidgetToFocus)
{
	if (!WidgetToFocus) return;
    
	// Отключаем управление камерой
	SetCameraControlEnabled(false);
    
	// Настраиваем режим GameAndUI - игра продолжает работать, но мышь видна и UI получает ввод
	FInputModeGameAndUI InputMode;
	InputMode.SetWidgetToFocus(WidgetToFocus->TakeWidget());
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputMode.SetHideCursorDuringCapture(false);
    
	SetInputMode(InputMode);
	bShowMouseCursor = true;
}

void AGamePlayerController::SetGameInputMode()
{
	// Включаем управление камерой
	SetCameraControlEnabled(true);
    
	// Возвращаем игровой режим ввода
	FInputModeGameOnly InputMode;
	SetInputMode(InputMode);
	bShowMouseCursor = false;
}

void AGamePlayerController::SetWebBrowserInputMode(UWebBrowser* WebBrowser)
{
    if (!WebBrowser) return;
    
    UE_LOG(LogTemp, Warning, TEXT("SetWebBrowserInputMode called"));
    
    // Отключаем управление камерой через наш флаг
    SetCameraControlEnabled(false);
    
    // ПОЛНОСТЬЮ отключаем ввод в контроллере
    SetIgnoreMoveInput(true);
    SetIgnoreLookInput(true);
    
    // Настраиваем режим ввода
    FInputModeGameAndUI InputMode;
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    InputMode.SetHideCursorDuringCapture(false);
    InputMode.SetWidgetToFocus(WebBrowser->TakeWidget());
    
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
    
    // Устанавливаем фокус на браузер
    WebBrowser->SetFocus();
    
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
    
	// Включаем управление камерой
	SetCameraControlEnabled(true);
    
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
    
	// Пересоздаем привязки, если нужно
	SetupInputComponent();
    
	UE_LOG(LogTemp, Warning, TEXT("Game input mode restored"));
}

void AGamePlayerController::BeginPlay()
{
	Super::BeginPlay();
}