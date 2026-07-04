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

	/** Хоткей (Space) для Start()/Stop() автомата прямо в PIE, через
	 *  UGameplayStatics::GetActorOfClass - а не через CallInEditor-кнопку в
	 *  Details panel. Кнопка ненадёжна во время PIE: если в момент клика в
	 *  Outliner всё ещё выбран актор из обычного (не-PIE) уровня, а не его
	 *  PIE-копия, вызов уйдёт на "замороженный" экземпляр, чей Tick() вообще
	 *  не крутится (LEVELTICK_ViewportsOnly) - симуляция внешне "не
	 *  запускается". Горячая клавиша так не ошибается: ввод всегда приходит
	 *  от PlayerController активного PIE-мира. */
	void OnToggleSimulation();

	/** Хоткей (F) для ручного одиночного шага (Next()) - только пока
	 *  непрерывная симуляция не запущена (IsSimulationRunning() == false);
	 *  иначе Next() и так откажется работать (гонка на Grid с фоновым
	 *  StepAsync(), см. bStepInProgress), но здесь проверяем заранее, чтобы
	 *  дать понятный лог вместо результата "как будто ничего не произошло". */
	void OnStepOnce();

	/** Хоткей (R) для GenerateRandom() - сбрасывает сетку в новое случайное
	 *  состояние с тем же Seed. В отличие от F, доступен и во время
	 *  непрерывной симуляции - GenerateRandom() сам разберётся с гонкой на
	 *  Grid через bStepInProgress, отдельная проверка здесь не нужна. */
	void OnResetSimulation();

	/** Хоткей (1) - включить освещённый режим (VIEWMODE LIT). Игра теперь
	 *  стартует в этом режиме по умолчанию (см. BeginPlay) - принудительный
	 *  Unlit больше не форсируется автоматически, а переключается вручную
	 *  через 1/2. */
	void OnSetLitMode();

	/** Хоткей (2) - включить безосветный режим (VIEWMODE UNLIT), тот же, что
	 *  раньше форсировался в BeginPlay - экономит на освещении при большом
	 *  числе инстансированных клеток автомата. */
	void OnSetUnlitMode();

	/** Создаются в рантайме через NewObject (см. SetupInputComponent()), а не
	 *  как Content-ассеты - для пары хоткеев на весь проект не нужны
	 *  отдельные .uasset. */
	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleSimulationAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> StepOnceAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ResetSimulationAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SetLitModeAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SetUnlitModeAction;

	UPROPERTY()
	TObjectPtr<class UInputMappingContext> SimulationMappingContext;

	UPROPERTY()
	AActor* CurrentViewTarget;
};
