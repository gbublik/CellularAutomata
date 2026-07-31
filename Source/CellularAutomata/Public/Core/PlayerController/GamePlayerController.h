// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Automata/Selection/SelectionCombineMode.h"
#include "GamePlayerController.generated.h"

class AAutomataOrchestrator;
class ARenderCullVolume;
class AGameCameraManager;

/**
 *
 */
UCLASS()
class CELLULARAUTOMATA_API AGamePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	/** Нужен ровно для одного: подменить класс камеры-менеджера на
	 *  AGameCameraManager (ортопроекция на NumPad 5). Именно конструктор, а не
	 *  BeginPlay(): APlayerController спавнит камеру-менеджер в
	 *  PostInitializeComponents(), к BeginPlay() менять класс уже поздно. */
	AGamePlayerController();

	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetCameraControlEnabled(bool bEnable);

	/** Включена ли сейчас ортогональная проекция (NumPad 5) - для FHudStats,
	 *  который зеркалит живые переключатели, а не хранит их копию. false, если
	 *  камеры-менеджера ещё нет (до BeginPlay/вне PIE). */
	UFUNCTION(BlueprintPure, Category = "Camera")
	bool IsOrthographicCamera() const;

	/** Кадрирует камеру на все живые клетки Orchestrator - код хоткея Home
	 *  (OnFrameAllCells()). НЕ вызывается автоматически из
	 *  AAutomataOrchestrator::ResetToInitialState() (хоткей R) - раньше R
	 *  сам кадрировал камеру по завершении сброса, но это было навязчиво
	 *  (сброс - частое, повторяющееся действие в процессе поиска интересных
	 *  паттернов, а не разовое событие вроде первого запуска), пользователь
	 *  явно попросил убрать; кадрирование теперь только по Home, отдельным
	 *  нажатием. Тоже НЕ вызывается из сохранения (SaveState()/SaveStateAs()) -
	 *  Save больше не трогает Grid/камеру вовсе, миниатюра снимает ровно тот
	 *  вид, что уже был на экране (см. AAutomataOrchestrator::WriteStateToFile()).
	 *  Подъезжает камерой вдоль текущего направления взгляда (не меняя ракурс, только
	 *  расстояние) так, чтобы вся сетка Orchestrator поместилась в кадр -
	 *  см. AAutomataOrchestrator::ComputeAliveCellsBounds()/FramingPadding.
	 *
	 *  bVisibleOnly (Shift+Home) - кадрировать не по всей сетке, а только по
	 *  тому, что СЕЙЧАС реально нарисовано (AAutomataOrchestrator::
	 *  ComputeVisibleCellsBounds() - те же три фильтра, что у рендера: куб
	 *  отсечения, возрастной фильтр, срез вдоль взгляда). Нужно ровно потому,
	 *  что обычный Home кадрирует на ВСЮ фигуру и обрезанным кубом кадром это
	 *  не заканчивается: большая часть структуры, отрезанная кубом, всё равно
	 *  тянет камеру на себя, и итоговый план оказывается издалека почти пустым.
	 *
	 *  false, если Orchestrator пуст, сетка пуста (или - при bVisibleOnly -
	 *  фильтры не оставили ни одной клетки), или пешка/камера ещё не готовы -
	 *  кадрировать нечего/некем. */
	bool FrameAllCells(AAutomataOrchestrator* Orchestrator, bool bVisibleOnly = false);

	/** То же кадрирование, но с ЗАДАННОГО направления взгляда: камера встаёт с
	 *  противоположной стороны сетки и разворачивается на неё (осевые ракурсы
	 *  нумпада, см. OnAlignCamera()). FrameAllCells() выше - тот же код с
	 *  текущим направлением и без поворота. bVisibleOnly - см. её doc-comment. */
	bool FrameAllCellsFromDirection(AAutomataOrchestrator* Orchestrator, const FVector& ViewDirection, bool bVisibleOnly = false);

	/** Включает/выключает единый режим взаимодействия мышкой - и с клетками
	 *  (драг-выделение, см. OnSelectDragStarted/Finished), и с HUD
	 *  (клик по кнопкам/панелям, см. UMainHudWidget) - Tab это одна и та же
	 *  "рука", а не два разных режима: оборачивает SetCameraControlEnabled()
	 *  (камера стоп/едет, курсор скрыт/показан) плюс сам флаг
	 *  bSelectionModeActive, который читают OnSelectDragStarted/Finished и
	 *  AAutomataOrchestrator::StartFromSelection() (выходит из режима после
	 *  извлечения). Ранее HUD-взаимодействие было отдельным режимом на
	 *  хоткее H (bHudInteractionModeActive) - убрано как лишнее различие:
	 *  Tab и так означает "курсор виден, камера не летает, кликаю мышкой". */
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetSelectionModeActive(bool bActive);

	bool IsSelectionModeActive() const { return bSelectionModeActive; }

	/** Для AGameHud::DrawHUD() - рамка выделения рисуется, пока тянется мышь;
	 *  текущая позиция мыши читается HUD'ом самостоятельно каждый кадр через
	 *  GetMousePosition(), сюда выносится только неподвижная стартовая точка. */
	bool IsDraggingSelection() const { return bIsDraggingSelection; }
	FVector2D GetSelectionDragStart() const { return DragStartScreenPos; }

	virtual void BeginPlay() override;

	/** Пока активен режим взаимодействия мышью, каждый кадр подгоняет экранный
	 *  размер манипулятора куба и, если ручку тянут, продолжает драг (у ЛКМ
	 *  есть только события нажатия/отпускания, само движение мыши между ними
	 *  надо опрашивать самим). Плюс разовая (на пешку) пересборка вертикальных
	 *  биндингов движения - см. RebindPawnVerticalMovement(). */
	virtual void Tick(float DeltaTime) override;

	/** Перехватывает P на уровне сырых оконных событий, в обход Enhanced
	 *  Input - см. подробный комментарий в реализации: при лагах (тяжёлый
	 *  AddInstances/перестройка HISM-дерева на игровом потоке) короткое
	 *  нажатие+отпускание P может целиком уместиться между двумя выборками
	 *  состояния Enhanced Input за кадр и никогда не сработать. */
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;

protected:
	bool bCanLookAround = false;

	virtual void SetupInputComponent() override;

	/** Заменяет движковый биндинг вертикального движения ADefaultPawn своим, без
	 *  клавиш, конфликтующих с хоткеями проекта.
	 *
	 *  ADefaultPawn вешает "вверх/вниз" (ось DefaultPawn_MoveUp) на шесть
	 *  клавиш, и две из них тут заняты: LeftControl роняет камеру на каждом
	 *  Ctrl+S/Ctrl+Shift+S/Ctrl+O/Ctrl+C, а C - на переключении отсечения
	 *  кубом, то есть камера уезжала вниз ровно в момент, когда смотришь, что
	 *  изменилось на экране.
	 *
	 *  Точечно выкинуть две клавиши нельзя: они лежат в приватном статическом
	 *  UPlayerInput::EngineDefinedAxisMappings, наружу отдан только const-
	 *  геттер. Поэтому снимаем биндинг оси целиком с InputComponent'а пешки и
	 *  вешаем свою ось на тот же ADefaultPawn::MoveUp_World - остаются Space/E
	 *  вверх и Q вниз, т.е. движковый набор минус два конфликта.
	 *
	 *  Зовётся из Tick(), а не из SetupInputComponent()/AcknowledgePossession():
	 *  InputComponent пешки создаётся позже них обоих, в PawnClientRestart()
	 *  (см. ClientRestart_Implementation - там AcknowledgePossession() стоит ДО
	 *  DispatchRestart()). Сравнение с VerticalMovementBoundPawn делает вызов
	 *  идемпотентным и заодно переигрывает пересборку, если сменится пешка. */
	void RebindPawnVerticalMovement();

	/** Пешка, для которой RebindPawnVerticalMovement() уже отработала.
	 *  UPROPERTY - иначе после реинстансинга Live Coding здесь остался бы мусор
	 *  (та же причина, что у AAutomataOrchestrator::GamePC). */
	UPROPERTY(Transient)
	TObjectPtr<APawn> VerticalMovementBoundPawn = nullptr;

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
	 *  нужна. Как и P (см. InputKey()), R НЕ маппится через Enhanced Input -
	 *  перехватывается напрямую в InputKey() по той же причине (см. её
	 *  doc-comment): под тяжёлым лагом игрового потока (тот же самый
	 *  AddInstances/перестройка HISM-дерева на больших сетках, из-за
	 *  которого P понадобилось выносить из Enhanced Input) короткое
	 *  нажатие+отпускание R могло целиком уместиться в один длинный кадр и
	 *  не сработать - именно это пользователь наблюдал ("не всегда
	 *  срабатывает"), тот же класс бага, что был у паузы до фикса. */
	void OnResetSimulation();

	/** Хоткей (N) - AAutomataOrchestrator::NewSeed(): перекатывает Seed и
	 *  заново засеивает сетку. Отличие от R (OnResetSimulation()): тот
	 *  повторяет ровно тот же исходный узор (InitialStateCells), а этот даёт
	 *  новый случайный - "не понравилось, покажи другое" одной клавишей, без
	 *  Details panel. Как P и R, ловится в InputKey() в обход Enhanced Input
	 *  (см. его doc-comment) и, как R, имеет отложенный путь на стороне
	 *  оркестратора (bNewSeedPending) - у клавиши было ровно те же два
	 *  независимых источника пропущенных нажатий. */
	void OnNewSeed();

	/** Хоткей (Y) - AAutomataOrchestrator::GenerateState(): строит начальное
	 *  состояние текущим геометрическим генератором. С зажатым Shift вместо
	 *  этого переключает тип генератора по кругу
	 *  (CycleStateGeneratorType()) - модификатор проверяется здесь, а не
	 *  маппингом, потому что Enhanced Input не умеет требовать его в привязке
	 *  клавиши (та же идиома, что Ctrl+S/Ctrl+C). Ловится в InputKey() в обход
	 *  Enhanced Input по той же причине, что P/R/N (см. его doc-comment):
	 *  клавишу жмут в момент худшего лага. */
	void OnGenerateState();

	/** Хоткей (F5) - показать/скрыть информационную панель HUD
	 *  (AAutomataOrchestrator::ToggleHudInfoPanel(), а оттуда событие в сам
	 *  виджет). Клавиша нужна потому, что UMG-виджет не получает нажатий сам:
	 *  ввод приходит сюда, в PlayerController. */
	void OnToggleHudInfoPanel();

	/** Хоткей (F6) - снять текущий вид как PNG-срез
	 *  (AAutomataOrchestrator::CaptureTextureSlice()); с зажатым Shift - через
	 *  диалог выбора файла. Это не скриншот: картинка растеризуется прямо из
	 *  сетки, поэтому не зависит ни от размера окна, ни от зума. */
	void OnCaptureTextureSlice();

	/** Хоткей (F7) - начать съёмку серии кадров по ходу симуляции, а если она
	 *  уже идёт, оборвать её (AAutomataOrchestrator::StartSeriesCapture()).
	 *  С зажатым Shift - не снимает, а выбирает следующий набор настроек съёмки
	 *  по кругу (CycleCapturePreset()), как Shift+Y к Y. */
	void OnToggleSeriesCapture();

	/** Общий обработчик хоткеев профилей рендера F1-F4 - просто находит
	 *  оркестратор и зовёт ApplyRenderPreset(PresetIndex). Четыре тонкие
	 *  обёртки ниже нужны потому, что BindAction принимает функцию без
	 *  аргументов - та же схема, что у OnMoveCullVolume() и стрелок. */
	void OnApplyRenderPreset(int32 PresetIndex);

	/** Хоткеи (F1-F4) - профили рендера (см. FRenderPreset и таблицу в
	 *  RenderPresets.cpp): Quality, Unlit, Balanced, Performance. Заменили
	 *  прежнюю пару "хоткей Lit / хоткей Unlit", которая с точки зрения
	 *  скорости не давала ничего: viewmode теперь лишь одна из настроек
	 *  профиля, рядом с тенями, отсечением, фоном и движковыми cvar'ами. */
	void OnApplyRenderPreset0();
	void OnApplyRenderPreset1();
	void OnApplyRenderPreset2();
	void OnApplyRenderPreset3();

	/** Хоткей (U) - показать/скрыть фон (небо и туман), не трогая освещение,
	 *  через AAutomataOrchestrator::SetBackgroundVisible()/IsBackgroundVisible().
	 *  Отдельно от профилей: "убрать фон" хочется и поверх Quality тоже. */
	void OnToggleBackground();

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

	/** Ctrl+C - показать/спрятать сам куб отсечения (ARenderCullVolume::
	 *  SetVolumeVisible()), НЕ выключая отсечение: коробка продолжает резать
	 *  клетки, просто не заслоняет то, что от них осталось. Голый C (см.
	 *  OnToggleRenderCullVolume() выше) - наоборот, выключает отсечение,
	 *  ничего не говоря о видимости. Вызывается из того же обработчика, что и
	 *  C: Enhanced Input не умеет требовать Ctrl в маппинге ключа, поэтому
	 *  модификатор проверяется в коде - та же идиома, что у Ctrl+S/Ctrl+O. */
	void OnToggleRenderCullVolumeVisibility();

	/** Хоткей (H, освободилась после слияния HUD-режима в Tab) - переключает
	 *  AAutomataOrchestrator::bEnableGhostShape через SetGhostShapeEnabled()/
	 *  IsGhostShapeEnabled() - показать/спрятать грубый chunk-силуэт вручную,
	 *  не трогая Details panel. */
	void OnToggleGhostShape();

	/** Хоткеи (+/-, основной ряд и NumPad) - меняют Speed автомата на
	 *  SpeedAdjustStep через AAutomataOrchestrator::AdjustSpeed(). */
	void OnIncreaseSpeed();
	void OnDecreaseSpeed();

	/** Хоткей (Home, и NumPad 0 - см. InputKey()) - резолвит AAutomataOrchestrator
	 *  и делегирует в публичный FrameAllCells() (см. её doc-comment для деталей
	 *  математики кадрирования) - тонкий обработчик. Shift+Home - тот же
	 *  FrameAllCells(bVisibleOnly=true): кадрировать по тому, что реально
	 *  видно (отсечено кубом/срезом/фильтром), а не по всей фигуре целиком. */
	void OnFrameAllCells();

	/** Хоткеи (T и G) - меняют StepsPerRender автомата на ±1 через
	 *  AAutomataOrchestrator::AdjustStepsPerRender(), привязаны на Triggered
	 *  (как +/- для Speed), так что удержание повторяет изменение каждый
	 *  кадр. С зажатым Shift не делают ничего - тогда работают
	 *  OnDoubleStepsPerRender()/OnHalveStepsPerRender() ниже. */
	void OnIncreaseStepsPerRender();
	void OnDecreaseStepsPerRender();

	/** Shift+T и Shift+G - переход к следующей/предыдущей СТЕПЕНИ ДВОЙКИ
	 *  (AAutomataOrchestrator::ScaleStepsPerRender()). Шаг в единицу до
	 *  интересных значений не доводит: набрать 256 с клавиши это либо 255
	 *  нажатий, либо ловля на удержании, где промахнуться проще, чем попасть
	 *  (наблюдалось - вместо 256 получилось 254). А интересны тут именно
	 *  степени двойки: и чистая картинка у фрактальных правил выпадает на
	 *  2^k, и потолок батчинга в FGpuComputeStrategy упирается в halo,
	 *  равный размеру батча.
	 *
	 *  Привязаны на Started, а не Triggered, в отличие от пары выше:
	 *  удвоение, повторяющееся каждый кадр удержания, улетело бы в потолок
	 *  за доли секунды. Shift проверяется прямо в обработчике - Enhanced
	 *  Input не умеет требовать модификатор в маппинге клавиши (тот же
	 *  идиом, что у Shift+F, Ctrl+S и Ctrl+C). */
	void OnDoubleStepsPerRender();
	void OnHalveStepsPerRender();

	/** Зажат ли любой Shift - левый или правый. */
	bool IsShiftHeld() const;

	/** Стрелки - двигают куб отсечения на клетку за нажатие: вверх/вниз по Y,
	 *  влево/вправо по X, с Shift вверх/вниз по Z.
	 *
	 *  Работают ТОЛЬКО в режиме выделения (Tab) и молча ничего не делают вне
	 *  него. Причина в конфликте: ADefaultPawn вешает на Up/Down полёт
	 *  вперёд-назад, а на Left/Right поворот камеры
	 *  (DefaultPawn.cpp, InitializeDefaultPawnInputBindings) - на голых
	 *  стрелках куб ездил бы одновременно с полётом. В режиме выделения ввод
	 *  пешки отключён (SetCameraControlEnabled(false)), стрелки свободны, и
	 *  это ровно тот режим, в котором куб и позиционируют: выделил клетку,
	 *  нажал K, дальше подвинул.
	 *
	 *  Triggered - удержание повторяет сдвиг, как у +/- для Speed. */
	void OnMoveCullVolume(const FIntVector& CellDelta);
	void OnMoveCullVolumeUp();
	void OnMoveCullVolumeDown();
	void OnMoveCullVolumeLeft();
	void OnMoveCullVolumeRight();

	/** Цифры 0-9 - фильтр по возрасту клетки (та же цифра ещё раз снимает
	 *  фильтр), причём 9 показывает возраст 9 И ВСЁ СТАРШЕ - иначе хвост рампы
	 *  не был бы виден ни под какой цифрой (см.
	 *  AAutomataOrchestrator::bAgeFilterIncludesOlder).
	 *
	 *  Shift+цифра ДОБАВЛЯЕТ возраст к уже показанным, а если он уже показан -
	 *  убирает его (AAutomataOrchestrator::ToggleAgeFilterValue()): по одному
	 *  слою за раз не сравнить положение фронта роста со старым ядром, а
	 *  вместе - видно сразу. Модификатор проверяется в самом обработчике -
	 *  Enhanced Input не умеет требовать его в привязке клавиши.
	 *
	 *  Ловятся в InputKey(), а не через Enhanced Input: десять клавиш одного
	 *  вида - это десять UInputAction ради одного switch. */
	void OnSetAgeFilter(int32 Age);

	/** Нумпад - позиционирование камеры; Home кадрирует по текущему ракурсу, а
	 *  это то же самое "сбоку": 1 - спереди, 3 - сзади, 4 - слева, 6 - справа,
	 *  8 - сверху, 2 - снизу, 7 - изометрия. Камера встаёт с противоположной от
	 *  направления взгляда стороны на расстоянии кадрирования и разворачивается
	 *  на сетку (FrameAllCellsFromDirection()).
	 *
	 *  ViewName - только для сообщения на экране: без него нажатие на пустой
	 *  сетке выглядит как несработавшая клавиша (та же причина, что у
	 *  ShowStatusMessage() вообще). bVisibleOnly (Shift) - см. doc-comment
	 *  FrameAllCells(); снимается в InputKey() один раз на все клавиши нумпада,
	 *  а не перечитывается в каждом обработчике. */
	void OnAlignCamera(const FVector& ViewDirection, const FString& ViewName, bool bVisibleOnly = false);

	/** NumPad 9 - тот же ракурс с противоположной стороны (взгляд
	 *  разворачивается на 180°). Полезнее ещё одной фиксированной оси: обойти
	 *  структуру, чтобы посмотреть, что у неё с обратной стороны, - обычный
	 *  жест, а какая именно ось сейчас смотрит в камеру, помнить не нужно.
	 *  Shift+NumPad 9 - bVisibleOnly, читает IsShiftHeld() сама (в отличие от
	 *  OnAlignCamera() выше, у неё нет вызывающего цикла, который снял бы
	 *  модификатор один раз заранее). */
	void OnAlignCameraToOppositeSide();

	/** NumPad 5 - ортопроекция вкл/выкл (AGameCameraManager::SetOrthographic()).
	 *  При включении ширина кадра сразу подгоняется под сетку: с шириной по
	 *  умолчанию ортопроекция показала бы либо одну клетку во весь экран, либо
	 *  пустоту, и это читалось бы как поломка, а не как смена проекции. */
	void OnToggleOrthographic();

	/** NumPad * и / - зум ортопроекции (ширина кадра, умножением - см.
	 *  AGameCameraManager::ScaleOrthoWidth()). Нужен потому, что в
	 *  ортопроекции полёт вперёд-назад на масштаб не влияет вовсе: W/S,
	 *  привычный "зум", там просто ничего не делают.
	 *
	 *  Работают и при выключенной ортопроекции (значение сохранится до её
	 *  включения) - сообщение об этом на экране по той же причине, что у [ / ]
	 *  при выключенном срезе: иначе клавиша выглядит сломанной. */
	void OnAdjustOrthoWidth(bool bZoomIn);

	/** NumPad . - кадрировать на ВЫДЕЛЕНИЕ (а не на всю сетку), сохранив
	 *  текущий ракурс. Пара к K/L: выделил интересный кусок - подъехал к нему,
	 *  не выискивая его глазами в структуре целиком. */
	void OnFrameSelection();

	/** Хоткей (J) - включает/выключает срез вдоль взгляда
	 *  (AAutomataOrchestrator::bEnableViewSlice). */
	void OnToggleViewSlice();

	/** Хоткеи ([ и ]) - двигают середину среза ближе/дальше от камеры; с
	 *  Shift меняют его толщину. Клавиши освободились, когда StepsPerRender
	 *  переехал на T/G. Привязаны к Triggered, как +/- для Speed: срез
	 *  подбирают на глаз, непрерывно, а не однократным нажатием. */
	void OnViewSliceNearer();
	void OnViewSliceFarther();

	/** Хоткей (Tab, изначально была C - перевязана рукой во время тестов) -
	 *  переключает единый режим взаимодействия мышкой - клетки и HUD разом
	 *  (см. SetSelectionModeActive()). */
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

	/** Хоткей (K) - AAutomataOrchestrator::MoveCullVolumeToSelection(),
	 *  телепортирует ARenderCullVolume к первой выделенной клетке - удобнее,
	 *  чем таскать гизмо куба вручную через весь уровень. */
	void OnMoveCullVolumeToSelection();

	/** Хоткей (L, соседствует с K по духу "куб <-> выделение") -
	 *  AAutomataOrchestrator::SelectCellsInCullVolume(), выделяет все живые
	 *  клетки внутри текущих границ ARenderCullVolume целиком. Модификатор
	 *  снимается в момент нажатия - той же идиомой, что и у
	 *  OnSelectDragStarted() (Ctrl приоритетнее Shift, если зажаты оба):
	 *  без модификатора - заменить выделение, Shift - добавить, Ctrl -
	 *  убрать. */
	void OnSelectCellsInCullVolume();

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
	 *  отдельные .uasset. Пауза (P) и сброс (R) не среди них - см. InputKey(). */
	UPROPERTY()
	TObjectPtr<class UInputAction> FastStepAction;

	/** По одному действию на профиль рендера (F1-F4). Массив, а не четыре
	 *  именованных поля: они полностью однотипны и различаются только
	 *  индексом, который тут же и уходит в ApplyRenderPreset(). */
	UPROPERTY()
	TArray<TObjectPtr<class UInputAction>> RenderPresetActions;

	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleBackgroundAction;

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
	TObjectPtr<class UInputAction> ToggleGhostShapeAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> MoveCullVolumeUpAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> MoveCullVolumeDownAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> MoveCullVolumeLeftAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> MoveCullVolumeRightAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ToggleViewSliceAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ViewSliceNearerAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> ViewSliceFartherAction;

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
	TObjectPtr<class UInputAction> MoveCullVolumeToSelectionAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SelectCellsInCullVolumeAction;

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

	/** Куб отсечения, за ручку которого сейчас тянут (см. ARenderCullVolume::
	 *  BeginGizmoDrag()). Пока он есть, ЛКМ занята манипулятором, а рамка
	 *  выделения не начинается вовсе - иначе одно нажатие делало бы сразу два
	 *  дела. */
	UPROPERTY(Transient)
	TObjectPtr<ARenderCullVolume> DraggedCullVolume;

	/** Резолвит ARenderCullVolume в мире (лениво, с ревалидацией) - тот же
	 *  идиом, что AAutomataOrchestrator::EnsureRenderCullVolume(). */
	ARenderCullVolume* FindCullVolume();

	UPROPERTY(Transient)
	TObjectPtr<ARenderCullVolume> CachedCullVolume;

	/** Ближайшая к лучу курсора точка на оси ручки, в мировых единицах вдоль
	 *  этой оси - то, что ARenderCullVolume ждёт в Begin/UpdateGizmoDrag().
	 *  Возвращает false, если луч почти параллелен оси (тогда точка уезжает в
	 *  бесконечность и драг дёргается). */
	bool ComputeGizmoAxisParam(const FVector& AxisOrigin, const FVector& Axis, FVector& OutRayOrigin, float& OutAxisParam) const;

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

	/** Шаг изменения среза вдоль взгляда за одно нажатие [ / ] - в мировых
	 *  единицах. Намеренно крупный: клетка по умолчанию 100 единиц, так что
	 *  шаг мельче просто не сдвинул бы срез на следующий слой клеток. */
	static constexpr float ViewSliceAdjustStep = 200.0f;

	/** Максимальный сдвиг мыши (в пикселях) между нажатием и отпусканием
	 *  ЛКМ, при котором жест считается кликом (выбор одиночной клетки под
	 *  курсором), а не драгом-прямоугольником - см. OnSelectDragFinished(). */
	static constexpr float ClickDragThresholdPixels = 4.0f;

	/** Запас поверх точного расстояния кадрирования (OnFrameAllCells()) -
	 *  без него сетка ровно касалась бы краёв кадра. */
	static constexpr float FramingPadding = 1.1f;

	/** Во сколько раз меняется ширина ортопроекции за одно нажатие * / /. */
	static constexpr float OrthoZoomStep = 1.25f;

	/** Общая часть всех кадрирований: ставит пешку на расстояние, с которого
	 *  сфера (Center, Radius) целиком влезает в кадр, и (если bApplyRotation)
	 *  разворачивает взгляд вдоль ViewDirection.
	 *
	 *  Питч клампится границами камеры-менеджера (ViewPitchMin/Max), и для
	 *  ПОЗИЦИИ берётся уже подрезанное направление, а не исходное: вид сверху -
	 *  это ровно -90°, которые камера-менеджер всё равно подрежет до -89.9 на
	 *  первом же движении мыши, и если поставить камеру строго по вертикали, а
	 *  смотреть на 0.1° мимо, центр кадра поедет. */
	bool FrameBounds(const FVector& Center, float Radius, const FVector& ViewDirection, bool bApplyRotation);

	/** Ширина ортопроекции, при которой сфера радиуса Radius видна целиком.
	 *  OrthoWidth задаёт ШИРИНУ кадра, а высота получается делением на
	 *  соотношение сторон - поэтому на широком экране (а он практически всегда
	 *  широкий) вписывать надо по высоте, иначе сетку обрежет сверху и снизу. */
	float ComputeOrthoWidthForRadius(float Radius) const;

	/** Камера-менеджер проекта; nullptr, если его ещё нет или (после правки
	 *  PlayerCameraManagerClass в Blueprint-наследнике) он другого класса. */
	AGameCameraManager* GetGameCameraManager() const;

	/** Сообщение поверх картинки о том, что сделала камера. Идёт через
	 *  AAutomataOrchestrator::ShowStatusMessage() (а не своим вызовом
	 *  AddOnScreenDebugMessage) намеренно: ключи там глобальные, и время жизни,
	 *  цвет и нумерация ключей должны оставаться в одном месте, иначе сообщения
	 *  о камере затирали бы чужие. */
	void ShowCameraStatusMessage(const FString& Message) const;

	/** Исходный MaxSpeed пешки до ускорения Shift'ом - 0 значит ещё не
	 *  закэширован (см. OnSpeedBoostStarted()/OnSpeedBoostEnded()). */
	float BaseFlySpeed = 0.0f;

	UPROPERTY()
	AActor* CurrentViewTarget;
};
