#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AutomataPhotoComponent.generated.h"

class AAutomataOrchestrator;

/** Парадный снимок (F10): останавливает прогон, применяет профиль съёмки,
 *  прячет инструменты редактирования, выдаёт HighResShot и по готовности файла
 *  отчитывается.
 *
 *  ПОЧЕМУ ОТДЕЛЬНЫЙ КОМПОНЕНТ, а не код оркестратора. Съёмка растянута во
 *  времени: команда возвращается мгновенно, а кадр рисуется движком позже, и
 *  кто-то обязан каждый кадр проверять, не появился ли файл. Раньше это делал
 *  тик АКТОРА - и вот в чём беда: тик оркестратора включён только когда идёт
 *  симуляция, быстрый шаг или живой срез (формула повторена в восьми местах),
 *  а снимают как раз на паузе. Приходилось насильно включать тик перед
 *  выстрелом и восстанавливать его по формуле после, то есть съёмка знала про
 *  условия, к ней отношения не имеющие, и ломалась бы от любой правки этой
 *  формулы. У компонента тик свой: включается на время съёмки, гасится по её
 *  окончании, и состояние актора при этом не трогается вовсе.
 *
 *  Второе - собственное состояние с временем жизни: метка выдачи команды и
 *  три флага "что было видно до съёмки". На акторе они лежали пятью
 *  транзиентными полями, ничем не связанными с остальными.
 *
 *  ЧТО ОСТАЛОСЬ НА ОРКЕСТРАТОРЕ, и это не мелочь: все настройки
 *  (PhotoShotResolution, PhotoShotDelayFrames, bPhotoLeanMemory) остаются его
 *  UPROPERTY, компонент читает их через владельца - ровно как поступил
 *  UAutomataSonifierComponent. Причина записана в docs/orchestrator.md:
 *  конфигурационное свойство, переехавшее в компонент, МОЛЧА теряет
 *  запечённое значение в NewMap.umap - ни ошибки компиляции, ни строчки в
 *  логе. Публичный TakePhotoShot() на акторе тоже остаётся (его зовут хоткей
 *  F10 и кнопка в Details-панели) и просто пробрасывает сюда. */
UCLASS(ClassGroup = (Automata), meta = (BlueprintSpawnableComponent))
class CELLULARAUTOMATA_API UAutomataPhotoComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAutomataPhotoComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** Снять кадр. Отказ (слишком большое разрешение, нет профиля съёмки в
	 *  таблице) не оставляет побочных эффектов - проверка идёт до того, как
	 *  что-либо остановлено или переключено. */
	void TakePhotoShot();

	/** Идёт ли съёмка прямо сейчас - для HUD и на случай повторного нажатия. */
	bool IsPhotoShotInProgress() const { return PhotoShotIssuedSeconds > 0.0; }

private:
	/** Оркестратор-владелец. Кэшируется, как Orchestrator у сонификатора, и по
	 *  той же причине - перебирать актёров тут незачем, владелец известен. */
	AAutomataOrchestrator* ResolveOrchestrator();

	/** Убирает из кадра коробку куба отсечения, её ручки и подсветку
	 *  выделения, запоминая, что из этого было видно. */
	void HideEditingVisuals(AAutomataOrchestrator& Orchestrator);

	/** Возвращает спрятанное HideEditingVisuals(). Зовётся безусловно, даже
	 *  когда файл не сохранился: экран обязан вернуться в то состояние, в
	 *  котором его оставил пользователь. */
	void RestoreEditingVisuals(AAutomataOrchestrator& Orchestrator);

	/** Ищет свежий PNG в папке снимков и печатает итог. Файл именно ИЩЕТСЯ, а
	 *  не предполагается: съёмка молча не сохраняет, например когда кадр не
	 *  влез в память, и "готово" без файла было бы худшим из отчётов. */
	void ReportCompleted(AAutomataOrchestrator& Orchestrator);

	/** Приводит PhotoShotResolution к допустимому и отказывает, если сторона
	 *  превышает предел RHI. Чистая проверка, без побочных эффектов. */
	bool ValidateResolution(AAutomataOrchestrator& Orchestrator, int32& OutWidth, int32& OutHeight) const;

	UPROPERTY(Transient)
	TObjectPtr<AAutomataOrchestrator> Orchestrator;

	/** Момент выдачи HighResShot. Ноль означает "съёмки нет" - он же признак
	 *  для IsPhotoShotInProgress() и условие в TickComponent(). Ставится ПЕРЕД
	 *  выдачей команды, хотя та возвращается мгновенно: сам кадр рисуется
	 *  позже, и отсчёт должен начинаться отсюда. */
	UPROPERTY(Transient)
	double PhotoShotIssuedSeconds = 0.0;

	/** Кадр выдачи команды - чтобы не принять за результат тот же самый кадр,
	 *  в котором съёмка была запущена. */
	UPROPERTY(Transient)
	uint64 PhotoShotIssuedFrame = 0;

	/** Что было видно до съёмки - чтобы вернуть ровно это, а не "всё". */
	UPROPERTY(Transient)
	bool bRestoreVolumeVisible = false;

	UPROPERTY(Transient)
	bool bRestoreGizmoVisible = false;

	UPROPERTY(Transient)
	bool bRestoreSelectionVisible = false;
};
