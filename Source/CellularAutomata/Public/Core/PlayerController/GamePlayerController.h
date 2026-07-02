// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "GamePlayerController.generated.h"

/**
 * 
 */
UCLASS()
class CELLULARAUTOMATA_API AGamePlayerController : public APlayerController
{
	GENERATED_BODY()
	
public:
	
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetCameraControlEnabled(bool bEnable);

	virtual void BeginPlay() override;

protected:
	bool bCanLookAround = false;

	virtual void SetupInputComponent() override;

	void RestoreGameInputMode();
	void DisableGameInputMode();

	UPROPERTY()
	AActor* CurrentViewTarget;
};
