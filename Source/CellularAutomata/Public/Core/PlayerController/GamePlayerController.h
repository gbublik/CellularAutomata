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

	/** Хоткей (F) - без Shift просто вызывает Next() один раз (обычный ручной
	 *  шаг, как и было изначально); если в момент нажатия зажат Shift -
	 *  вместо одного шага включается непрерывный автошаг "как Play"
	 *  (AAutomataOrchestrator::StartFastStep(), темп по Speed), который
	 *  работает только пока F физически зажата и останавливается по
	 *  отпусканию (см. OnFastStepReleased()). Привязана на
	 *  ETriggerEvent::Started (однократно на нажатие) и Completed (на
	 *  отпускание, нужно только для hold-режима Shift+F) - не Triggered,
	 *  иначе голый F повторял бы Next() каждый кадр, пока зажата, как раньше.
	 *  Работает только пока непрерывная симуляция (P) не запущена -
	 *  AAutomataOrchestrator::StartFastStep() и так откажется работать в
	 *  этом случае (см. её реализацию), здесь дублируем проверку заранее
	 *  только чтобы дать понятный лог вместо результата "как будто ничего
	 *  не произошло". */
	void OnFastStepPressed();

	/** Останавливает автошаг Shift+F, если он сейчас активен
	 *  (AAutomataOrchestrator::IsFastStepActive()) - обычный одиночный шаг
	 *  (F без Shift) не запускает никакого длящегося состояния, поэтому
	 *  отпускание клавиши в этом случае ничего не делает. */
	void OnFastStepReleased();

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

	/** Хоткей (Left Shift, удержание) - ускоряет полёт камеры на время
	 *  удержания. Камера летает через ADefaultPawn/UFloatingPawnMovement -
	 *  масштабируем MaxSpeed на AAutomataOrchestrator::CameraSpeedMultiplier.
	 *  BaseFlySpeed кэширует исходную скорость при первом нажатии (0 значит
	 *  ещё не закэширована), чтобы OnSpeedBoostEnded() мог её восстановить
	 *  не накапливая ошибку при повторных нажатиях. */
	void OnSpeedBoostStarted();
	void OnSpeedBoostEnded();

	/** Хоткей (Z) - включает/выключает разлитый по кадрам рендер
	 *  (AAutomataOrchestrator::SetChunkedRenderEnabled()/IsChunkedRenderEnabled()). */
	void OnToggleChunkedRender();

	/** Хоткей (X) - переключает порядок реавила разлитого по кадрам рендера
	 *  на следующий по кругу (AAutomataOrchestrator::CycleChunkedRenderOrder(),
	 *  см. EChunkedRenderOrder) - чтобы подобрать, как клетки появляются по
	 *  кадрам (блобами/равномерно/от камеры/от центра), не открывая Details
	 *  panel. */
	void OnCycleChunkedRenderOrder();

	/** Хоткеи (+/-, основной ряд и NumPad) - меняют Speed автомата на
	 *  SpeedAdjustStep через AAutomataOrchestrator::AdjustSpeed(). */
	void OnIncreaseSpeed();
	void OnDecreaseSpeed();

	/** Хоткей (Home) - подъезжает камерой вдоль текущего направления взгляда
	 *  (не меняя ракурс, только расстояние) так, чтобы вся сетка автомата
	 *  поместилась в кадр. Использует AAutomataOrchestrator::
	 *  ComputeAliveCellsBounds() (описанная сфера вокруг живых клеток) и
	 *  текущий FOV камеры (PlayerCameraManager->GetFOVAngle()) - расстояние
	 *  считается из условия, что сфера радиуса R видна целиком под углом
	 *  FOV/2 (Distance = R / sin(FOV/2)), с небольшим запасом
	 *  (FramingPadding), чтобы клетки не упирались точно в край кадра. */
	void OnFrameAllCells();

	/** Хоткеи (T и G) - меняют StepsPerRender автомата на ±1 через
	 *  AAutomataOrchestrator::AdjustStepsPerRender(), привязаны на Triggered
	 *  (как +/- для Speed), так что удержание повторяет изменение каждый
	 *  кадр. */
	void OnIncreaseStepsPerRender();
	void OnDecreaseStepsPerRender();

	/** Создаются в рантайме через NewObject (см. SetupInputComponent()), а не
	 *  как Content-ассеты - для пары хоткеев на весь проект не нужны
	 *  отдельные .uasset. */
	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleSimulationAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> FastStepAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ResetSimulationAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SetLitModeAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SetUnlitModeAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SpeedBoostAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleChunkedRenderAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> CycleChunkedRenderOrderAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> IncreaseSpeedAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> DecreaseSpeedAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> FrameAllCellsAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> IncreaseStepsPerRenderAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> DecreaseStepsPerRenderAction;

	UPROPERTY()
	TObjectPtr<class UInputMappingContext> SimulationMappingContext;

	/** Шаг изменения Speed за одно нажатие +/-. */
	static constexpr float SpeedAdjustStep = 0.5f;

	/** Запас поверх точного расстояния кадрирования (OnFrameAllCells()) -
	 *  без него сетка ровно касалась бы краёв кадра. */
	static constexpr float FramingPadding = 1.1f;

	/** Исходный MaxSpeed пешки до ускорения Shift'ом - 0 значит ещё не
	 *  закэширован (см. OnSpeedBoostStarted()/OnSpeedBoostEnded()). */
	float BaseFlySpeed = 0.0f;

	UPROPERTY()
	AActor* CurrentViewTarget;
};
