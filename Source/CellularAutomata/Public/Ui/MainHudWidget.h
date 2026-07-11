#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MainHudWidget.generated.h"

class AAutomataOrchestrator;

/**
 * Базовый C++ класс главного HUD-виджета (см. план "HUD: архитектура и
 * Phase 1"). Один виджет-"shell" на всю игру, показывается через
 * существующий FUiController/AAutomataOrchestrator::HUDWidgetClass -
 * визуал (статус-бар, кнопки, панели) собирается Blueprint-наследником в
 * UMG Designer, этот класс даёт только резолв оркестратора и
 * Blueprint-доступные данные (GetOrchestrator()/GetOrchestrator()->
 * GetHudStats()).
 *
 * Резолвит AAutomataOrchestrator сам через GetActorOfClass() (тот же
 * идиом, что весь проект - ARenderCullVolume, AGamePlayerController и
 * т.д.), а не через ручную ссылку, назначаемую в редакторе на самом
 * виджете - виджет создаётся FUiController'ом с владельцем-PlayerController,
 * без прямой связи с оркестратором (см. CLAUDE.md про "Illegal TEXT
 * reference" - ассеты/виджеты не могут держать постоянную ссылку на
 * конкретный актёр уровня, назначенную в редакторе).
 */
UCLASS()
class CELLULARAUTOMATA_API UMainHudWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Резолвит и кэширует AAutomataOrchestrator в мире виджета - лениво,
	 *  ревалидирует IsValid() на каждый вызов на случай, если оркестратор
	 *  ещё не заспавнен или был уничтожен (тот же идиом, что
	 *  AAutomataOrchestrator::EnsureRenderCullVolume()). Возвращает nullptr,
	 *  если оркестратора в мире нет - Blueprint-граф должен проверять на
	 *  None перед использованием, как и везде в проекте. */
	UFUNCTION(BlueprintPure, Category = "Automata|HUD")
	AAutomataOrchestrator* GetOrchestrator();

protected:
	virtual void NativeConstruct() override;

private:
	UPROPERTY(Transient)
	AAutomataOrchestrator* CachedOrchestrator = nullptr;
};
