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

	/** Нажали клавишу показа/скрытия информационной панели (F5, см.
	 *  AGamePlayerController::OnToggleHudInfoPanel()).
	 *
	 *  Реализуется графом в Blueprint-наследнике: C++ намеренно не знает, что
	 *  именно прячется - одна панель, несколько или цикл "всё -> компактно ->
	 *  ничего". Вёрстка HUD живёт в UMG, и решать, что такое "информационная
	 *  панель", должна она же. Событие нужно потому, что виджет не получает
	 *  нажатий клавиш сам: ввод приходит в PlayerController, а до виджета его
	 *  надо донести. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Automata|HUD")
	void OnToggleInfoPanel();

	/** Статусное сообщение хоткея - см. AAutomataOrchestrator::
	 *  ShowStatusMessage(), она сюда и направляет.
	 *
	 *  Раньше это шло в движковый канал (AddOnScreenDebugMessage), а он рисует
	 *  строки от жёстко зашитых 45 пикселей сверху (UnrealEngine.cpp,
	 *  MessageStartY - ни настройки, ни cvar) - ровно поверх верхней панели
	 *  HUD. Здесь сообщение становится частью HUD: место, стиль и время жизни
	 *  решает вёрстка, а не движок.
	 *
	 *  Key - постоянный идентификатор ВИДА сообщения (EStatusMessageKey), а не
	 *  порядковый номер: он затем и заведён, чтобы повторное сообщение той же
	 *  категории заменяло предыдущее, а не копилось - хоткеи на Triggered
	 *  срабатывают каждый кадр удержания. В графе по нему удобно решать, в
	 *  какую строку писать.
	 *
	 *  Пока событие не реализовано в Blueprint-наследнике, оркестратор этого не
	 *  видит как ошибку и продолжает слать в движковый канал - см. проверку
	 *  владельца UFunction в ShowStatusMessage(). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Automata|HUD")
	void OnStatusMessage(const FText& Message, int32 Key);

protected:
	virtual void NativeConstruct() override;

private:
	UPROPERTY(Transient)
	AAutomataOrchestrator* CachedOrchestrator = nullptr;
};
