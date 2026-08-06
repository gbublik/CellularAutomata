#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

/** Все клавиши проекта одним списком - и те, что идут через Enhanced Input
 *  (таблица в SetupInputComponent()), и те, что ловятся сырым InputKey().
 *
 *  Зачем перечисление, а не клавиша прямо в коде: клавиши настраиваются из
 *  Config/DefaultInput.ini, и настраиваться должны ОБЕ половины. Половин две не
 *  по недосмотру - Enhanced Input опрашивает уровень клавиши раз в тик и теряет
 *  короткое нажатие внутри одного лагового кадра, поэтому пробел, R, N, Y,
 *  цифры и нумпад намеренно уведены в InputKey() (см. его doc-comment). Клавиш
 *  там БОЛЬШЕ, чем в Enhanced Input, и это ровно те, которые жмут чаще всего, -
 *  так что настройка, покрывающая только маппинги, покрыла бы меньшую и менее
 *  важную половину раскладки.
 *
 *  Значение по умолчанию у каждой клавиши остаётся в коде (HotkeyRegistry::
 *  GetDefaults()), а ini его только переопределяет. Это не подстраховка от
 *  опечатки, а условие того, что проект собирается и работает без единой
 *  строчки конфига: пустой ini - это раскладка по умолчанию, а не отсутствие
 *  управления. */
enum class EHotkey : uint8
{
	// --- Через Enhanced Input (таблица в SetupInputComponent()) ---
	FastStep,
	RenderPreset0,
	RenderPreset1,
	RenderPreset2,
	RenderPreset3,
	ToggleBackground,
	SpeedBoost,
	ToggleChunkedRender,
	CycleChunkedRenderOrder,
	ToggleWaitForChunkedRenderToFinish,
	ToggleCellCulling,
	ToggleRenderCullVolume,
	ToggleGhostShape,
	IncreaseSpeed,
	IncreaseSpeedNumPad,
	DecreaseSpeed,
	DecreaseSpeedNumPad,
	FrameAllCells,
	IncreaseStepsPerRender,
	DecreaseStepsPerRender,
	MoveCullVolumeUp,
	MoveCullVolumeDown,
	MoveCullVolumeLeft,
	MoveCullVolumeRight,
	ToggleViewSlice,
	ViewSliceNearer,
	ViewSliceFarther,
	ToggleSelectionMode,
	SelectDrag,
	EraseCell,
	ExtractSelection,
	InvertSelection,
	BakeCellsToMesh,
	DeleteSelectedCells,
	MoveCullVolumeToSelection,
	SelectCellsInCullVolume,
	SaveState,
	LoadState,
	ArrayCells,
	CopyCells,
	PasteCells,

	// --- Через сырой InputKey() ---
	ToggleSimulation,
	ResetSimulation,
	NewSeed,
	GenerateState,
	UndoRedo,
	ToggleHudInfoPanel,
	TakePhotoShot,
	ToggleSonification,
	CaptureTextureSlice,
	ToggleSeriesCapture,

	AgeFilter0,
	AgeFilter1,
	AgeFilter2,
	AgeFilter3,
	AgeFilter4,
	AgeFilter5,
	AgeFilter6,
	AgeFilter7,
	AgeFilter8,
	AgeFilter9,

	ToggleOrthographic,
	FrameAllCellsFromNumPad,
	AlignCameraToOppositeSide,
	FrameSelection,
	OrthoZoomIn,
	OrthoZoomOut,
	ViewLeft,
	ViewRight,
	ViewTop,
	ViewBottom,
	ViewFront,
	ViewBack,
	ViewIsometric,

	Count
};

/** Свободные функции над таблицей клавиш - тот же идиом, что RulePresets/
 *  RenderPresets/CapturePresets и остальные namespace'ы проекта: таблица-
 *  константа, GetAll()-подобный доступ, никакого актора внутри. Логика
 *  разрешения имён и коллизий тестируется headless (см. docs/testing.md). */
namespace HotkeyRegistry
{
	/** Одна клавиша по умолчанию: имя, под которым она ищется в
	 *  Config/DefaultInput.ini, и клавиша, если её там нет. */
	struct FHotkeyDefault
	{
		EHotkey Hotkey;
		/** Имя ActionMapping'а в ini. С префиксом CA_, чтобы в общем списке
		 *  Project Settings -> Input клавиши проекта не путались с чужими. */
		const TCHAR* ActionName;
		FKey DefaultKey;
		/** Действие срабатывает только с модификатором, поэтому делить клавишу
		 *  с другим - норма, а не конфликт (Ctrl+Z против голой Z). Проверка
		 *  конфликтов такие пары пропускает; без флага она ругалась бы на них
		 *  всегда, а предупреждение, которое всегда горит, никто не читает. */
		bool bModifierGuarded = false;
	};

	/** Таблица по умолчанию, в порядке EHotkey. Ровно EHotkey::Count строк -
	 *  проверяется тестом Input.HotkeyRegistry, а не только глазами. */
	CELLULARAUTOMATA_API const TArray<FHotkeyDefault>& GetDefaults();

	/** Имя ActionMapping'а для клавиши (то, что видно в Project Settings ->
	 *  Input -> Bindings). NAME_None для неизвестного значения. */
	CELLULARAUTOMATA_API FName GetActionName(EHotkey Hotkey);

	/** Разрешает всю раскладку: берёт клавишу из ActionMappings, а если её там
	 *  нет - значение по умолчанию. Возвращает массив длиной EHotkey::Count,
	 *  индексируемый значением EHotkey.
	 *
	 *  Читает UInputSettings напрямую, а не UPlayerInput::GetKeysForAction():
	 *  настройки доступны в любой момент, в том числе до того, как у
	 *  контроллера появился PlayerInput, а сама раскладка нужна уже в
	 *  SetupInputComponent().
	 *
	 *  ActionMappings - штатный движковый механизм (UPROPERTY(config,
	 *  EditAnywhere) на UInputSettings): текст в Config/DefaultInput.ini, то
	 *  есть видно в git diff, и правится в редакторе через Project Settings ->
	 *  Input без кода и пересборки. Взят как ИСТОЧНИК ДАННЫХ - привязка
	 *  обработчиков по-прежнему своя (Enhanced Input либо InputKey()), легаси-
	 *  диспетчеризация не используется. */
	CELLULARAUTOMATA_API TArray<FKey> ResolveKeys();

	/** Клавиши, назначенные больше чем одному действию, - собранные по именам
	 *  действий. Пусто, если конфликтов нет.
	 *
	 *  Существует потому, что ini правится руками, а конфликт в нём не даёт ни
	 *  ошибки компиляции, ни отказа: обе клавиши просто срабатывают вместе, и
	 *  выглядит это как "хоткей делает что-то лишнее". Вызывается один раз при
	 *  разрешении раскладки и пишет предупреждение в лог. */
	CELLULARAUTOMATA_API TMap<FKey, TArray<FName>> FindConflicts(const TArray<FKey>& ResolvedKeys);
}
