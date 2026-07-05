// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "GameHud.generated.h"

/**
 * Нативный HUD (без UMG) - единственная сегодняшняя задача: рисовать
 * полупрозрачную рамку драг-выделения клеток (AGamePlayerController::
 * IsDraggingSelection()/GetSelectionDragStart()), пока пользователь тянет
 * ЛКМ в режиме выделения. Текущая позиция мыши читается заново каждый кадр
 * через GetOwningPlayerController()->GetMousePosition() - не кэшируется,
 * контроллеру не нужно проталкивать обновления через Tick.
 */
UCLASS()
class CELLULARAUTOMATA_API AGameHud : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
