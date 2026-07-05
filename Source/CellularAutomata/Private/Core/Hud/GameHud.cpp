// Fill out your copyright notice in the Description page of Project Settings.

#include "Core/Hud/GameHud.h"
#include "Core/PlayerController/GamePlayerController.h"

void AGameHud::DrawHUD()
{
	Super::DrawHUD();

	AGamePlayerController* GamePC = Cast<AGamePlayerController>(GetOwningPlayerController());
	if (!GamePC || !GamePC->IsDraggingSelection())
	{
		return;
	}

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	if (!GamePC->GetMousePosition(MouseX, MouseY))
	{
		return;
	}

	const FVector2D DragStart = GamePC->GetSelectionDragStart();
	const FVector2D CurrentPos(MouseX, MouseY);
	const FVector2D RectMin(FMath::Min(DragStart.X, CurrentPos.X), FMath::Min(DragStart.Y, CurrentPos.Y));
	const FVector2D RectMax(FMath::Max(DragStart.X, CurrentPos.X), FMath::Max(DragStart.Y, CurrentPos.Y));

	DrawRect(FLinearColor(0.0f, 1.0f, 1.0f, 0.15f), RectMin.X, RectMin.Y, RectMax.X - RectMin.X, RectMax.Y - RectMin.Y);
}
