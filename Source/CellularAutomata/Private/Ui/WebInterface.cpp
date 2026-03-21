#include "CellularAutomata/Public/Ui/WebInterface.h"

// Определяем статическую константу
const FString UWebInterface::BindingName = TEXT("ue");

void UWebInterface::OnButtonClicked(const FString& ButtonName)
{
    UE_LOG(LogTemp, Log, TEXT("WebInterface: Button clicked: %s"), *ButtonName);
    OnMessageFromWeb.Broadcast(ButtonName);
}

void UWebInterface::OnJsonData(const FString& JsonString)
{
    UE_LOG(LogTemp, Log, TEXT("WebInterface: JSON received: %s"), *JsonString);
    OnMessageFromWeb.Broadcast(JsonString);
}

void UWebInterface::SendToWeb(const FString& EventName, const FString& Data)
{
    FString Script = FString::Printf(
        TEXT("window.dispatchEvent(new CustomEvent('%s', { detail: %s }));"), 
        *EventName, 
        *Data
    );
    ExecuteJavaScript(Script);
}

void UWebInterface::BindToBrowser(UWebBrowser* InWebBrowser)
{
    if (!InWebBrowser) return;

    WebBrowser = TWeakObjectPtr<UWebBrowser>(InWebBrowser);
    
    // 1. Получаем TSharedRef на базовый виджет
    TSharedRef<SWidget> RawWidget = InWebBrowser->TakeWidget();

    WebBrowser->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
    
    // 2. Приводим тип к SWebBrowser
    // Используем StaticCastSharedRef, так как на входе ссылка (SharedRef)
    SlateBrowser = StaticCastSharedRef<SWebBrowser>(RawWidget);

    if (SlateBrowser.IsValid())
    {
        // 3. Биндим объект
        SlateBrowser->BindUObject(BindingName, this);
        UE_LOG(LogTemp, Log, TEXT("WebInterface: Bound successfully"));
    }
}

void UWebInterface::ExecuteJavaScript(const FString& ScriptText)
{
    if (WebBrowser.IsValid())
    {
        WebBrowser->ExecuteJavascript(ScriptText);
    }
}