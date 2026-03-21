// WebInterface.h
#pragma once

#include "CoreMinimal.h"
#include "WebBrowser.h"
#include "SWebBrowser.h" 
#include "IWebBrowserAdapter.h"
#include "WebInterface.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMessageFromWeb, const FString&, Message);

UCLASS(BlueprintType)
class CELLULARAUTOMATA_API UWebInterface : public UObject
{
	GENERATED_BODY()

public:
	// Функции, вызываемые из JavaScript
	UFUNCTION()
	void OnButtonClicked(const FString& ButtonName);
    
	UFUNCTION()
	void OnJsonData(const FString& JsonString);
    
	// Отправка данных в JavaScript
	UFUNCTION(BlueprintCallable, Category = "Web Interface")
	void SendToWeb(const FString& EventName, const FString& Data);
    
	// Событие для Blueprints/C++
	UPROPERTY(BlueprintAssignable, Category = "Web Interface")
	FOnMessageFromWeb OnMessageFromWeb;
    
	// Привязка к браузеру через AddBinding
	void BindToBrowser(UWebBrowser* InWebBrowser);
    
	// Выполнить JavaScript
	UFUNCTION(BlueprintCallable, Category = "Web Interface")
	void ExecuteJavaScript(const FString& ScriptText);

	// Имя, под которым объект будет доступен в JavaScript
	static const FString BindingName;

	TSharedPtr<SWebBrowser> SlateBrowser;
	
	UWebBrowser* GetWebBrowser() const { return WebBrowser.Get(); }
private:
	TWeakObjectPtr<UWebBrowser> WebBrowser;
};