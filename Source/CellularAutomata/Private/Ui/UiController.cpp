#include "CellularAutomata/Public/Ui/UiController.h"

#include "GameFramework/PlayerController.h"
#include "Blueprint/UserWidget.h"

FUiController::FUiController(APlayerController* InPC)
    : PlayerController(InPC)
    , HUDWidget(nullptr)
    , HUDWidgetClass(nullptr)
    , bCreationAttempted(false)
{
}

FUiController::~FUiController()
{
    // Автоматически очищаем виджет при уничтожении контроллера
    if (HUDWidget.IsValid())
    {
        HUDWidget->RemoveFromParent();
    }
}

void FUiController::SetHUDClass(TSubclassOf<UUserWidget> InHUDClass)
{
    HUDWidgetClass = InHUDClass;
    // Сбрасываем флаг, чтобы при следующем вызове создать новый виджет
    bCreationAttempted = false;
}

bool FUiController::IsPlayerControllerValid() const
{
    if (!PlayerController.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("FUiController: PlayerController is invalid"));
        return false;
    }
    return true;
}

void FUiController::CreateHUDIfNeeded()
{
    if (bCreationAttempted || HUDWidget.IsValid())
    {
        return;
    }
    
    bCreationAttempted = true;
    
    if (!IsPlayerControllerValid())
    {
        UE_LOG(LogTemp, Error, TEXT("FUiController: Cannot create HUD - invalid PlayerController"));
        return;
    }
    
    if (!HUDWidgetClass)
    {
        UE_LOG(LogTemp, Error, TEXT("FUiController: Cannot create HUD - HUDWidgetClass not set"));
        return;
    }
    
    // Создаем виджет
    NewWidget = CreateWidget<UUserWidget>(PlayerController.Get(), HUDWidgetClass);
    
    if (NewWidget)
    {
        HUDWidget = NewWidget;
        
        // Ищем WebBrowser в виджете
        UWebBrowser* Browser = Cast<UWebBrowser>(HUDWidget->GetWidgetFromName(FName("WebBrowser")));

        if (Browser)
        {
            WebInterface = NewObject<UWebInterface>();
            WebInterface->BindToBrowser(Browser);   
            UE_LOG(LogTemp, Log, TEXT("FUiController: HUD created and WebInterface bound successfully"));
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("FUiController: WebBrowser not found in HUD"));
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("FUiController: Failed to create HUD widget"));
    }
}

void FUiController::ShowHUD()
{
    CreateHUDIfNeeded();
    
    if (!HUDWidget.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("FUiController: Cannot show HUD - widget is invalid"));
        return;
    }
    
    if (!HUDWidget->IsInViewport())
    {
        HUDWidget->AddToViewport();
        UE_LOG(LogTemp, Verbose, TEXT("FUiController: HUD shown"));
    }
}

void FUiController::HideHUD() const
{
    if (HUDWidget.IsValid() && HUDWidget->IsInViewport())
    {
        HUDWidget->RemoveFromParent();
        UE_LOG(LogTemp, Verbose, TEXT("FUiController: HUD hidden"));
    }
}

void FUiController::ToggleHUD()
{
    if (IsHUDVisible())
    {
        HideHUD();
    }
    else
    {
        ShowHUD();
    }
}

bool FUiController::IsHUDVisible() const
{
    return HUDWidget.IsValid() && HUDWidget->IsInViewport();
}

bool FUiController::IsHUDValid() const
{
    return HUDWidget.IsValid();
}