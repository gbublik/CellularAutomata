#include "Automata/Rendering/RenderCullVolume.h"
#include "Components/BoxComponent.h"

#if WITH_EDITOR
#include "Orchestration/AutomataOrchestrator.h"
#include "Kismet/GameplayStatics.h"
#endif

ARenderCullVolume::ARenderCullVolume()
{
	PrimaryActorTick.bCanEverTick = false;

	BoundsBox = CreateDefaultSubobject<UBoxComponent>(TEXT("BoundsBox"));
	SetRootComponent(BoundsBox);

	BoundsBox->SetBoxExtent(FVector(50.0f, 50.0f, 50.0f));
	BoundsBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BoundsBox->SetGenerateOverlapEvents(false);
	// Видим и в PIE, не только в редакторе - удобно подгонять границы куба
	// по живой картинке, не только по вьюпорту редактора.
	BoundsBox->SetHiddenInGame(false);
}

FBox ARenderCullVolume::GetWorldBounds() const
{
	const FVector Origin = BoundsBox->GetComponentLocation();
	const FVector Extent = BoundsBox->GetScaledBoxExtent();
	return FBox(Origin - Extent, Origin + Extent);
}

#if WITH_EDITOR
void ARenderCullVolume::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	// bFinished==false - промежуточные тики драга, ещё не отпустили мышь;
	// перерисовывать на каждый из них незачем (та же дорогая AddInstances,
	// от которой мы и пытаемся уйти) - ждём, пока драг реально завершится.
	if (bFinished)
	{
		NotifyOrchestratorToRefresh();
	}
}

void ARenderCullVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	NotifyOrchestratorToRefresh();
}

void ARenderCullVolume::NotifyOrchestratorToRefresh() const
{
	if (AAutomataOrchestrator* Orchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass())))
	{
		Orchestrator->RefreshRenderCullVolume();
	}
}
#endif
