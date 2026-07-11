#include "Ui/MainHudWidget.h"
#include "Orchestration/AutomataOrchestrator.h"
#include "Kismet/GameplayStatics.h"

void UMainHudWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Не обязательно (GetOrchestrator() и сам лениво резолвит при первом
	// обращении), но так Blueprint-граф виджета может сразу использовать
	// GetOrchestrator() в своём Construct-событии, не тратя первый тик на
	// пустой результат.
	GetOrchestrator();
}

AAutomataOrchestrator* UMainHudWidget::GetOrchestrator()
{
	if (!IsValid(CachedOrchestrator))
	{
		CachedOrchestrator = Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(GetWorld(), AAutomataOrchestrator::StaticClass()));
	}
	return CachedOrchestrator;
}
