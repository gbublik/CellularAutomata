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

	/** Хоткей (P) для Start()/Stop() автомата прямо в PIE, через
	 *  UGameplayStatics::GetActorOfClass - а не через CallInEditor-кнопку в
	 *  Details panel. Кнопка ненадёжна во время PIE: если в момент клика в
	 *  Outliner всё ещё выбран актор из обычного (не-PIE) уровня, а не его
	 *  PIE-копия, вызов уйдёт на "замороженный" экземпляр, чей Tick() вообще
	 *  не крутится (LEVELTICK_ViewportsOnly) - симуляция внешне "не
	 *  запускается". Горячая клавиша так не ошибается: ввод всегда приходит
	 *  от PlayerController активного PIE-мира. */
	void OnToggleSimulation();

	/** Создаются в рантайме через NewObject (см. SetupInputComponent()), а не
	 *  как Content-ассеты - для одного хоткея на весь проект не нужен
	 *  отдельный .uasset. */
	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleSimulationAction;

	UPROPERTY()
	TObjectPtr<class UInputMappingContext> SimulationMappingContext;

	UPROPERTY()
	AActor* CurrentViewTarget;
};
