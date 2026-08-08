// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Automata/Photo/AutomataPhotoComponent.h"

// Съёмка целиком живёт в UAutomataPhotoComponent - здесь остались только точка
// входа и создание компонента. Причина переезда в его doc-comment, если коротко:
// снимок растянут во времени, и проверять его готовность надо каждый кадр, а
// тик актора включён только когда идёт симуляция, быстрый шаг или живой срез -
// снимают же обычно на паузе. У компонента тик свой.

UAutomataPhotoComponent* AAutomataOrchestrator::EnsurePhotoComponent()
{
	if (IsValid(PhotoComponent))
	{
		return PhotoComponent;
	}

	PhotoComponent = NewObject<UAutomataPhotoComponent>(this, TEXT("AutomataPhoto"));
	if (PhotoComponent)
	{
		PhotoComponent->RegisterComponent();
	}
	return PhotoComponent;
}

void AAutomataOrchestrator::TakePhotoShot()
{
	// Обёртка остаётся на акторе, а не переезжает в компонент: её зовут хоткей
	// F10 и кнопка CallInEditor в Details-панели, и оба нашли бы функцию на
	// компоненте, только если знать про него - а знать про него им незачем.
	if (UAutomataPhotoComponent* Component = EnsurePhotoComponent())
	{
		Component->TakePhotoShot();
	}
}

void AAutomataOrchestrator::TakePanoramaShot()
{
	// Тот же компонент, что и обычный снимок, и по той же причине: панорама -
	// это фотография, у неё тот же обряд подготовки (остановить прогон,
	// применить профиль съёмки, убрать из кадра инструменты и вернуть их
	// обратно). Заводить под неё второй компонент значило бы завести второй
	// friend к оркестратору ради того же самого набора приватных членов.
	if (UAutomataPhotoComponent* Component = EnsurePhotoComponent())
	{
		Component->TakePanoramaShot();
	}
}
