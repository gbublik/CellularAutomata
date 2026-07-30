// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Camera/PlayerCameraManager.h"
#include "GameCameraManager.generated.h"

/**
 *  Камера-менеджер проекта. Всё, что он добавляет к APlayerCameraManager, -
 *  переключаемая ОРТОГОНАЛЬНАЯ проекция (NumPad 5, см.
 *  AGamePlayerController::OnToggleOrthographic()): без перспективных искажений
 *  структура автомата читается как чертёж, слои и симметрии видно сразу, а с
 *  осевыми ракурсами нумпада (NumPad 1/3/4/6/8/2) это ровно то, чем в
 *  DCC-редакторах разглядывают форму.
 *
 *  Почему отдельный класс, а не UCameraComponent на пешке: пешка тут -
 *  стандартный ADefaultPawn, камеры-компонента у него нет вовсе, вид берётся
 *  из GetActorEyesViewPoint(). А режим проекции живёт в FMinimalViewInfo,
 *  которую камера-менеджер пересобирает КАЖДЫЙ кадр, так что записать
 *  ProjectionMode в PlayerCameraManager->ViewTarget.POV снаружи нельзя -
 *  следующий DoUpdateCamera() его затрёт. UpdateViewTarget() ровно та
 *  виртуальная точка, где вид уже посчитан, но ещё не ушёл в рендер, и
 *  вызывается она и для текущей цели вида, и для входящей (при блендинге), так
 *  что проекция не мигает на переключении цели.
 *
 *  Ширину кадра (OrthoWidth) подгоняет под сетку контроллер при кадрировании
 *  (AGamePlayerController::FrameBounds()) - здесь только хранение и кламп:
 *  радиус живых клеток знает оркестратор, а не камера.
 */
UCLASS()
class CELLULARAUTOMATA_API AGameCameraManager : public APlayerCameraManager
{
	GENERATED_BODY()

public:
	virtual void UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime) override;

	bool IsOrthographic() const { return bOrthographic; }
	void SetOrthographic(bool bEnable) { bOrthographic = bEnable; }

	/** Ширина кадра в мировых единицах. В ортопроекции масштаб задаёт именно
	 *  она, а не расстояние до объекта: отъезжать и подъезжать камерой
	 *  бесполезно, картинка не меняется вовсе.
	 *
	 *  Сеттер - путь ПОДГОНКИ под сетку (кадрирование), флаг ручного зума он не
	 *  поднимает; ручной зум - ScaleOrthoWidth() ниже. */
	float GetOrthoWidth() const { return OrthoWidth; }
	void SetOrthoWidth(float NewOrthoWidth);

	/** Зум в ортопроекции - умножение, а не прибавление шага: интересные
	 *  масштабы тут отличаются на порядки (клетка - 100 единиц, структура -
	 *  сотни тысяч), и аддитивный шаг был бы одновременно слишком грубым на
	 *  одном конце и слишком мелким на другом.
	 *
	 *  Поднимает флаг ручного зума (HasUserOrthoWidth()) - см. его. */
	void ScaleOrthoWidth(float Multiplier);

	/** Пользователь сам выставил масштаб клавишами * / / - и его не надо
	 *  затирать подгонкой под сетку.
	 *
	 *  Без этого флага смена ракурса (нумпад 1/3/4/6/8/2/7/9) сбрасывала
	 *  накрученный зум: осевые виды идут через то же кадрирование, что Home, а
	 *  оно в ортопроекции обязано выставлять ширину - иначе в ортопроекции не
	 *  делало бы вообще ничего (расстояние-то на картинку не влияет). Получалось,
	 *  что подобрать масштаб и обойти структуру по осям - взаимоисключающие
	 *  действия, о чём и был отчёт.
	 *
	 *  Флаг снимают только явные просьбы "вписать в кадр": Home/Shift+Home,
	 *  NumPad 0, NumPad . и включение ортопроекции. Смена ракурса - не просьба
	 *  вписать, а просьба посмотреть с другой стороны, и масштаб не трогает. */
	bool HasUserOrthoWidth() const { return bOrthoWidthUserSet; }
	void ClearUserOrthoWidth() { bOrthoWidthUserSet = false; }

private:
	/** UPROPERTY(Transient), а не плайн-члены: так состояние переживает
	 *  реинстансинг классов при Live Coding (та же причина, что у
	 *  AAutomataOrchestrator::GamePC) - иначе горячая правка посреди PIE
	 *  молча возвращала бы камеру в перспективу. */
	UPROPERTY(Transient)
	bool bOrthographic = false;

	UPROPERTY(Transient)
	float OrthoWidth = 10000.0f;

	/** См. HasUserOrthoWidth(). Transient по той же причине, что и поля выше. */
	UPROPERTY(Transient)
	bool bOrthoWidthUserSet = false;

	/** Глубина видимого слоя в ортопроекции (мировые единицы). Слой
	 *  СИММЕТРИЧЕН относительно камеры - см. UpdateViewTarget(). */
	static constexpr float OrthoDepthRange = 1000000.0f;

	static constexpr float MinOrthoWidth = 10.0f;
	static constexpr float MaxOrthoWidth = 100000000.0f;
};