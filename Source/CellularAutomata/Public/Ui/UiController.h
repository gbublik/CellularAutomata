#pragma once

#include "CoreMinimal.h"

class APlayerController;
class UUserWidget;

/**
 * Внутренний класс для управления HUD
 * Полностью инкапсулирует логику создания и управления виджетом
 */
class CELLULARAUTOMATA_API FUiController
{
public:
	explicit FUiController(APlayerController* InPC);
	~FUiController();

	// Запрещаем копирование
	FUiController(const FUiController&) = delete;
	FUiController& operator=(const FUiController&) = delete;

	// Основные методы управления HUD
	void ShowHUD();
	void HideHUD() const;
	void ToggleHUD();
    
	// Настройка класса виджета (можно вызвать до CreateHUD)
	void SetHUDClass(TSubclassOf<UUserWidget> InHUDClass);
    
	// Проверка состояния
	bool IsHUDVisible() const;
	bool IsHUDValid() const;

	/** Текущий виджет HUD, либо nullptr, если он ещё не создан или уже
	 *  уничтожен. Нужен вызывающим, которым надо обратиться к самому виджету
	 *  (например, сообщить ему о нажатии клавиши - см.
	 *  AAutomataOrchestrator::ToggleHudInfoPanel()). Отдаётся через геттер, а
	 *  не через публичное поле NewWidget: тот сырой указатель ничего не знает
	 *  о времени жизни виджета, а HUDWidget - слабая ссылка и честно вернёт
	 *  nullptr после уничтожения. */
	UUserWidget* GetWidget() const;
	
	
	UUserWidget* NewWidget;
private:
	// Создание HUD (ленивая инициализация)
	void CreateHUDIfNeeded();
    
	// Внутренние проверки
	bool IsPlayerControllerValid() const;

	TWeakObjectPtr<APlayerController> PlayerController;
	TWeakObjectPtr<UUserWidget> HUDWidget;
    
	// Храним класс для создания виджета
	TSubclassOf<UUserWidget> HUDWidgetClass;
    
	// Флаг для отслеживания попытки создания
	bool bCreationAttempted;
};