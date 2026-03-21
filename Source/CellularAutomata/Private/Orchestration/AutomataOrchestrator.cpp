// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Orchestration/GamePlayerController.h"


// Sets default values
AAutomataOrchestrator::AAutomataOrchestrator()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = false;
}

// Called when the game starts or when spawned
void AAutomataOrchestrator::BeginPlay()
{
	Super::BeginPlay();
	InitializeHUD();
	InitializePlayerController();
}

// Called every frame
void AAutomataOrchestrator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void AAutomataOrchestrator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
}

void AAutomataOrchestrator::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
}

void AAutomataOrchestrator::NewSeed()
{
	
}

void AAutomataOrchestrator::InitializeHUD()
{
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		UiController = MakeUnique<FUiController>(PC);
		UiController->SetHUDClass(HUDWidgetClass);
		
		UiController->ShowHUD();
	}
}

void AAutomataOrchestrator::InitializePlayerController()
{
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (AGamePlayerController* GamePC = Cast<AGamePlayerController>(PC))
		{
			// Получаем WebBrowser из UiController
			if (UiController && UiController->WebInterface)
			{
				UWebBrowser* Browser = UiController->WebInterface->GetWebBrowser();
				if (Browser)
				{
					// Используем специальный метод для WebBrowser
					GamePC->SetWebBrowserInputMode(Browser);
					//GamePC->SetUIInputMode(UiController->NewWidget);
					UE_LOG(LogTemp, Warning, TEXT("WebBrowser input mode set"));
				}
				else
				{
					// Fallback на обычный UI режим
				}
			}
			else
			{
				// Если нет WebInterface, используем обычный режим
				GamePC->SetUIInputMode(UiController->NewWidget);
			}
            
			UE_LOG(LogTemp, Warning, TEXT("GamePlayerController setup complete"));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Wrong PlayerController class! Using: %s"), *PC->GetClass()->GetName());
		}
	}
}

void AAutomataOrchestrator::PostActorCreated()
{
	Super::PostActorCreated();
	InitializeHUD();
}

void AAutomataOrchestrator::GenerateRandom()
{
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		
	}
}

void AAutomataOrchestrator::Next()
{
	
}

void AAutomataOrchestrator::Start()
{
	
}

void AAutomataOrchestrator::Pause()
{
	
}

void AAutomataOrchestrator::Stop()
{
	
}

void AAutomataOrchestrator::Clear()
{
	
}