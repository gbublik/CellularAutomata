// Fill out your copyright notice in the Description page of Project Settings.


#include "Core/GameMode/AGameMode.h"
#include "Core/PlayerController/GamePlayerController.h"

AAGameMode::AAGameMode()
{
	PlayerControllerClass = AGamePlayerController::StaticClass();
}
