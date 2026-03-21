// Fill out your copyright notice in the Description page of Project Settings.


#include "Orchestration/AGameMode.h"

AAGameMode::AAGameMode()
{
	PlayerControllerClass = AGamePlayerController::StaticClass();
}