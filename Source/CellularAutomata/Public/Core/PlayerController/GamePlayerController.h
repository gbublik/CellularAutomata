// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Automata/Selection/SelectionCombineMode.h"
#include "GamePlayerController.generated.h"

class AAutomataOrchestrator;

/**
 *
 */
UCLASS()
class CELLULARAUTOMATA_API AGamePlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetCameraControlEnabled(bool bEnable);

	/** Кадрирует камеру на все живые клетки Orchestrator - общий код хоткея
	 *  Home (OnFrameAllCells()) и авто-кадрирования после
	 *  AAutomataOrchestrator::ResetToInitialState() (хоткей R - в конце
	 *  сброса камера сама встаёт на результат, как по Home). НЕ вызывается
	 *  из сохранения (SaveState()/SaveStateAs()) - Save больше не трогает
	 *  Grid/камеру вовсе, миниатюра снимает ровно тот вид, что уже был на
	 *  экране (см. AAutomataOrchestrator::WriteStateToFile()). Подъезжает
	 *  камерой вдоль текущего направления взгляда (не меняя ракурс, только
	 *  расстояние) так, чтобы вся сетка Orchestrator поместилась в кадр -
	 *  см. AAutomataOrchestrator::ComputeAliveCellsBounds()/FramingPadding.
	 *  false, если Orchestrator пуст, сетка пуста, или пешка/камера ещё не
	 *  готовы - кадрировать нечего/некем. */
	bool FrameAllCells(AAutomataOrchestrator* Orchestrator);

	/** Включает/выключает режим выделения клеток мышкой - оборачивает
	 *  SetCameraControlEnabled() (камера стоп/едет, курсор скрыт/показан),
	 *  плюс сам флаг bSelectionModeActive, который читают обработчики
	 *  драг-выделения (OnSelectDragStarted/Finished) и AAutomataOrchestrator::
	 *  StartFromSelection() (выходит из режима выделения после извлечения). */
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetSelectionModeActive(bool bActive);

	bool IsSelectionModeActive() const { return bSelectionModeActive; }

	/** Для AGameHud::DrawHUD() - рамка выделения рисуется, пока тянется мышь;
	 *  текущая позиция мыши читается HUD'ом самостоятельно каждый кадр через
	 *  GetMousePosition(), сюда выносится только неподвижная стартовая точка. */
	bool IsDraggingSelection() const { return bIsDraggingSelection; }
	FVector2D GetSelectionDragStart() const { return DragStartScreenPos; }

	virtual void BeginPlay() override;

	/** Перехватывает P на уровне сырых оконных событий, в обход Enhanced
	 *  Input - см. подробный комментарий в реализации: при лагах (тяжёлый
	 *  AddInstances/перестройка HISM-дерева на игровом потоке) короткое
	 *  нажатие+отпускание P может целиком уместиться между двумя выборками
	 *  состояния Enhanced Input за кадр и никогда не сработать. */
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;

protected:
	bool bCanLookAround = false;

	virtual void SetupInputComponent() override;

	void RestoreGameInputMode();
	void DisableGameInputMode();

	/** Хоткей (Space) для Start()/Stop() автомата прямо в PIE, через
	 *  UGameplayStatics::GetActorOfClass - а не через CallInEditor-кнопку в
	 *  Details panel. Кнопка ненадёжна во время PIE: если в момент клика в
	 *  Outliner всё ещё выбран актор из обычного (не-PIE) уровня, а не его
	 *  PIE-копия, вызов уйдёт на "замороженный" экземпляр, чей Tick() вообще
	 *  не крутится (LEVELTICK_ViewportsOnly) - симуляция внешне "не
	 *  запускается". Горячая клавиша так не ошибается: ввод всегда приходит
	 *  от PlayerController активного PIE-мира. */
	void OnToggleSimulation();

	/** Хоткей (F) - без Shift просто вызывает Next() один раз (обычный ручной
	 *  шаг, как и было изначально); если в момент нажатия зажат Shift -
	 *  вместо одного шага включается непрерывный автошаг "как Play"
	 *  (AAutomataOrchestrator::StartFastStep(), темп по Speed), который
	 *  работает только пока F физически зажата и останавливается по
	 *  отпусканию (см. OnFastStepReleased()). Привязана на
	 *  ETriggerEvent::Started (однократно на нажатие) и Completed (на
	 *  отпускание, нужно только для hold-режима Shift+F) - не Triggered,
	 *  иначе голый F повторял бы Next() каждый кадр, пока зажата, как раньше.
	 *  Работает только пока непрерывная симуляция (P) не запущена -
	 *  AAutomataOrchestrator::StartFastStep() и так откажется работать в
	 *  этом случае (см. её реализацию), здесь дублируем проверку заранее
	 *  только чтобы дать понятный лог вместо результата "как будто ничего
	 *  не произошло". */
	void OnFastStepPressed();

	/** Останавливает автошаг Shift+F, если он сейчас активен
	 *  (AAutomataOrchestrator::IsFastStepActive()) - обычный одиночный шаг
	 *  (F без Shift) не запускает никакого длящегося состояния, поэтому
	 *  отпускание клавиши в этом случае ничего не делает. */
	void OnFastStepReleased();

	/** Хоткей (R) для AAutomataOrchestrator::ResetToInitialState() - если
	 *  ранее было извлечено выделение (Enter, см. OnExtractSelection()),
	 *  сбрасывает сетку обратно на этот сохранённый паттерн (а не на новый
	 *  случайный) - иначе (если ни разу не извлекали выделение) ведёт себя
	 *  как раньше, вызывая GenerateRandom() под капотом. В отличие от F,
	 *  доступен и во время непрерывной симуляции - оба пути сами разберутся
	 *  с гонкой на Grid через bStepInProgress, отдельная проверка здесь не
	 *  нужна. */
	void OnResetSimulation();

	/** Хоткей (1) - включить освещённый режим (VIEWMODE LIT). Игра теперь
	 *  стартует в этом режиме по умолчанию (см. BeginPlay) - принудительный
	 *  Unlit больше не форсируется автоматически, а переключается вручную
	 *  через 1/2. */
	void OnSetLitMode();

	/** Хоткей (2) - включить безосветный режим (VIEWMODE UNLIT), тот же, что
	 *  раньше форсировался в BeginPlay - экономит на освещении при большом
	 *  числе инстансированных клеток автомата. */
	void OnSetUnlitMode();

	/** Хоткей (Left Shift, удержание) - ускоряет полёт камеры на время
	 *  удержания. Камера летает через ADefaultPawn/UFloatingPawnMovement -
	 *  масштабируем MaxSpeed на AAutomataOrchestrator::CameraSpeedMultiplier.
	 *  BaseFlySpeed кэширует исходную скорость при первом нажатии (0 значит
	 *  ещё не закэширована), чтобы OnSpeedBoostEnded() мог её восстановить
	 *  не накапливая ошибку при повторных нажатиях. */
	void OnSpeedBoostStarted();
	void OnSpeedBoostEnded();

	/** Хоткей (Z) - включает/выключает разлитый по кадрам рендер
	 *  (AAutomataOrchestrator::SetChunkedRenderEnabled()/IsChunkedRenderEnabled()). */
	void OnToggleChunkedRender();

	/** Хоткей (X) - переключает порядок реавила разлитого по кадрам рендера
	 *  на следующий по кругу (AAutomataOrchestrator::CycleChunkedRenderOrder(),
	 *  см. EChunkedRenderOrder) - чтобы подобрать, как клетки появляются по
	 *  кадрам (блобами/равномерно/от камеры/от центра), не открывая Details
	 *  panel. */
	void OnCycleChunkedRenderOrder();

	/** Хоткей (V) - переключает AAutomataOrchestrator::bWaitForChunkedRenderToFinish
	 *  через SetWaitForChunkedRenderToFinish()/IsWaitingForChunkedRenderToFinish() -
	 *  выбор между "прервать недорисованный разлив и сразу перерисовать новое
	 *  состояние" (по умолчанию) и "дождаться, пока текущий разлив
	 *  дорисуется, и только потом считать/рисовать следующее". */
	void OnToggleWaitForChunkedRenderToFinish();

	/** Хоткей (B) - переключает AAutomataOrchestrator::bEnableCellCulling
	 *  через SetCellCullingEnabled()/IsCellCullingEnabled() - включает/
	 *  выключает отсечение клеток по расстоянию, не трогая подобранные
	 *  CellCullStartDistance/CellCullEndDistance. */
	void OnToggleCellCulling();

	/** Хоткей (C) - переключает AAutomataOrchestrator::bEnableRenderCullVolume
	 *  через SetRenderCullVolumeEnabled()/IsRenderCullVolumeEnabled() -
	 *  включает/выключает отсечение клеток вне ARenderCullVolume ДО
	 *  построения инстансов (в отличие от OnToggleCellCulling() выше,
	 *  который переключает пост-хок отсечение по расстоянию на уже
	 *  построенных инстансах). */
	void OnToggleRenderCullVolume();

	/** Хоткеи (+/-, основной ряд и NumPad) - меняют Speed автомата на
	 *  SpeedAdjustStep через AAutomataOrchestrator::AdjustSpeed(). */
	void OnIncreaseSpeed();
	void OnDecreaseSpeed();

	/** Хоткей (Home) - резолвит AAutomataOrchestrator и делегирует в
	 *  публичный FrameAllCells() (см. её doc-comment для деталей математики
	 *  кадрирования) - тонкий обработчик, вся логика теперь переиспользуема
	 *  и вызывается также из AAutomataOrchestrator::ResetToInitialState(). */
	void OnFrameAllCells();

	/** Хоткеи (T и G) - меняют StepsPerRender автомата на ±1 через
	 *  AAutomataOrchestrator::AdjustStepsPerRender(), привязаны на Triggered
	 *  (как +/- для Speed), так что удержание повторяет изменение каждый
	 *  кадр. */
	void OnIncreaseStepsPerRender();
	void OnDecreaseStepsPerRender();

	/** Хоткей (Tab, изначально была C - перевязана рукой во время тестов) -
	 *  переключает режим выделения клеток мышкой (см. SetSelectionModeActive()). */
	void OnToggleSelectionMode();

	/** ЛКМ, Started/Completed - работают только пока bSelectionModeActive.
	 *  OnSelectDragStarted() запоминает экранную точку старта драга и снимает
	 *  зажатый модификатор (Shift - добавить к выделению, Ctrl - убрать из
	 *  него, без модификатора - заменить, см. ESelectionCombineMode) в
	 *  PendingSelectionCombineMode; OnSelectDragFinished() различает клик и
	 *  драг по сдвигу мыши (ClickDragThresholdPixels): клик выбирает
	 *  одиночную клетку под курсором (депроецированный луч ->
	 *  AAutomataOrchestrator::SelectCellUnderCursor()), драг строит итоговый
	 *  прямоугольник (старт vs. текущая позиция мыши) и передаёт его вместе с
	 *  этим режимом в AAutomataOrchestrator::SelectCellsInScreenRect() вместе
	 *  с матрицей вида-проекции, посчитанной один раз на всю операцию (не на
	 *  клетку). */
	void OnSelectDragStarted();
	void OnSelectDragFinished();

	/** Хоткей (Enter) - AAutomataOrchestrator::StartFromSelection(), делает
	 *  выделенные клетки единственным содержимым новой сетки. */
	void OnExtractSelection();

	/** Хоткей (I) - AAutomataOrchestrator::InvertSelection(), инвертирует
	 *  выделение относительно живых клеток. Как и Enter, не гейтится на
	 *  bSelectionModeActive - выделение существует независимо от того,
	 *  включён ли сейчас режим мышиного выделения. */
	void OnInvertSelection();

	/** Хоткей (M) - AAutomataOrchestrator::BakeCellsToMesh(), запекает
	 *  текущее состояние (или выделение, если оно есть) в один цельный меш
	 *  и выгружает клетки из памяти - снимок-"скульптура" для осмотра. */
	void OnBakeCellsToMesh();

	/** Хоткей (Delete) - AAutomataOrchestrator::DeleteSelectedCells(),
	 *  убивает выделенные клетки прямо в текущей сетке (ручная правка
	 *  состояния, симуляция не сбрасывается). */
	void OnDeleteSelectedCells();

	/** Хоткей S (Ctrl+S / Ctrl+Shift+S, стандартная комбинация) -
	 *  AAutomataOrchestrator::SaveState()/SaveStateAs(). Действие на клавише
	 *  S привязано БЕЗ модификатора (Enhanced Input не умеет требовать Ctrl
	 *  прямо в маппинге ключа), поэтому голый S по-прежнему уходит камере
	 *  (движение назад, DefaultPawn) - обработчик сам проверяет Ctrl через
	 *  IsInputKeyDown() и молча выходит, если Ctrl не зажат (та же идиома,
	 *  что у Shift-модификатора OnFastStepPressed() и Ctrl/Shift в
	 *  OnSelectDragStarted()). При зажатом Ctrl: без Shift - "Сохранить"
	 *  (SaveState(), тихая перезапись последнего пути), с Shift - "Сохранить
	 *  как" (SaveStateAs(), всегда диалог). */
	void OnSaveOrSaveAs();

	/** Хоткей O (Ctrl+O, стандартная комбинация) -
	 *  AAutomataOrchestrator::LoadStateFromFile(). Как и у S, маппинг сам
	 *  ключа без модификатора - Ctrl проверяется внутри обработчика. */
	void OnLoadState();

	/** Создаются в рантайме через NewObject (см. SetupInputComponent()), а не
	 *  как Content-ассеты - для пары хоткеев на весь проект не нужны
	 *  отдельные .uasset. Пауза (P) больше не среди них - см. InputKey(). */
	UPROPERTY()
	TObjectPtr<class UInputAction> FastStepAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ResetSimulationAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SetLitModeAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SetUnlitModeAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SpeedBoostAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleChunkedRenderAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> CycleChunkedRenderOrderAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleWaitForChunkedRenderToFinishAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleCellCullingAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleRenderCullVolumeAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> IncreaseSpeedAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> DecreaseSpeedAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> FrameAllCellsAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> IncreaseStepsPerRenderAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> DecreaseStepsPerRenderAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleSelectionModeAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SelectDragAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ExtractSelectionAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> InvertSelectionAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> BakeCellsToMeshAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> DeleteSelectedCellsAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SaveStateAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> LoadStateAction;

	UPROPERTY()
	TObjectPtr<class UInputMappingContext> SimulationMappingContext;

	/** true, пока пользователь удерживает ЛКМ после OnSelectDragStarted() и
	 *  до OnSelectDragFinished() - читается AGameHud::DrawHUD() через
	 *  IsDraggingSelection(). */
	bool bIsDraggingSelection = false;

	/** Экранная точка старта текущего драг-выделения (пиксели, viewport-
	 *  relative), зафиксированная в OnSelectDragStarted(). */
	FVector2D DragStartScreenPos = FVector2D::ZeroVector;

	/** Режим комбинирования текущего драг-выделения с уже выделенным (Shift -
	 *  добавить, Ctrl - убрать, иначе заменить) - снимается по зажатым
	 *  модификаторам в момент старта драга (OnSelectDragStarted()), а не
	 *  отпускания, и передаётся в SelectCellsInScreenRect() на mouse-up. */
	ESelectionCombineMode PendingSelectionCombineMode = ESelectionCombineMode::Replace;

	/** true между OnToggleSelectionMode()-включением и выключением - гейтит
	 *  OnSelectDragStarted()/OnSelectDragFinished(), чтобы ЛКМ не запускала
	 *  выделение вне явно включённого режима. */
	bool bSelectionModeActive = false;

	/** Шаг изменения Speed за одно нажатие +/-. */
	static constexpr float SpeedAdjustStep = 0.5f;

	/** Максимальный сдвиг мыши (в пикселях) между нажатием и отпусканием
	 *  ЛКМ, при котором жест считается кликом (выбор одиночной клетки под
	 *  курсором), а не драгом-прямоугольником - см. OnSelectDragFinished(). */
	static constexpr float ClickDragThresholdPixels = 4.0f;

	/** Запас поверх точного расстояния кадрирования (OnFrameAllCells()) -
	 *  без него сетка ровно касалась бы краёв кадра. */
	static constexpr float FramingPadding = 1.1f;

	/** Исходный MaxSpeed пешки до ускорения Shift'ом - 0 значит ещё не
	 *  закэширован (см. OnSpeedBoostStarted()/OnSpeedBoostEnded()). */
	float BaseFlySpeed = 0.0f;

	UPROPERTY()
	AActor* CurrentViewTarget;
};
