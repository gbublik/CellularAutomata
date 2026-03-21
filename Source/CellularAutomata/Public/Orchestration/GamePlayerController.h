// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "WebBrowser.h"
#include "Blueprint/UserWidget.h"
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
    
	// Переключить режим ввода для UI
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetUIInputMode(UUserWidget* WidgetToFocus);
    
	// Вернуть игровой режим ввода
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetGameInputMode();

	virtual void BeginPlay() override;
	void SetWebBrowserInputMode(UWebBrowser* WebBrowser);
	void RestoreGameInputMode();
protected:
	bool bCanLookAround = false;
    
	virtual void SetupInputComponent() override;
	void Turn(float Value);
	void LookUp(float Value);
};
