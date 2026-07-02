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