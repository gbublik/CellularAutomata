#include "Core/PlayerController/HotkeyRegistry.h"

#include "GameFramework/InputSettings.h"

namespace HotkeyRegistry
{
	const TArray<FHotkeyDefault>& GetDefaults()
	{
		// Строится один раз: FKey не constexpr (EKeys::* - это static const
		// объекты), поэтому статической таблицей на этапе компиляции её не
		// сделать, а порядок инициализации статиков между единицами трансляции
		// не определён. Локальный статик внутри функции решает и то, и другое.
		static const TArray<FHotkeyDefault> Defaults =
		{
			// --- Через Enhanced Input ---
			{ EHotkey::FastStep,                           TEXT("CA_FastStep"),                           EKeys::F },
			{ EHotkey::RenderPreset0,                      TEXT("CA_RenderPreset0"),                      EKeys::F1 },
			{ EHotkey::RenderPreset1,                      TEXT("CA_RenderPreset1"),                      EKeys::F2 },
			{ EHotkey::RenderPreset2,                      TEXT("CA_RenderPreset2"),                      EKeys::F3 },
			{ EHotkey::RenderPreset3,                      TEXT("CA_RenderPreset3"),                      EKeys::F4 },
			{ EHotkey::ToggleBackground,                   TEXT("CA_ToggleBackground"),                   EKeys::U },
			{ EHotkey::SpeedBoost,                         TEXT("CA_SpeedBoost"),                         EKeys::LeftShift },
			{ EHotkey::ToggleChunkedRender,                TEXT("CA_ToggleChunkedRender"),                EKeys::Z },
			{ EHotkey::CycleChunkedRenderOrder,            TEXT("CA_CycleChunkedRenderOrder"),            EKeys::X },
			{ EHotkey::ToggleWaitForChunkedRenderToFinish, TEXT("CA_ToggleWaitForChunkedRender"),         EKeys::V },
			{ EHotkey::ToggleCellCulling,                  TEXT("CA_ToggleCellCulling"),                  EKeys::B },
			{ EHotkey::ToggleRenderCullVolume,             TEXT("CA_ToggleRenderCullVolume"),             EKeys::C },
			{ EHotkey::ToggleGhostShape,                   TEXT("CA_ToggleGhostShape"),                   EKeys::H },
			// Основной ряд и нумпад - двумя отдельными строками, а не одним
			// действием на две клавиши: в ini одно имя это одна клавиша, и
			// раздельные строки заодно позволяют переназначить их порознь.
			{ EHotkey::IncreaseSpeed,                      TEXT("CA_IncreaseSpeed"),                      EKeys::Equals },
			{ EHotkey::IncreaseSpeedNumPad,                TEXT("CA_IncreaseSpeedNumPad"),                EKeys::Add },
			{ EHotkey::DecreaseSpeed,                      TEXT("CA_DecreaseSpeed"),                      EKeys::Hyphen },
			{ EHotkey::DecreaseSpeedNumPad,                TEXT("CA_DecreaseSpeedNumPad"),                EKeys::Subtract },
			{ EHotkey::FrameAllCells,                      TEXT("CA_FrameAllCells"),                      EKeys::Home },
			{ EHotkey::IncreaseStepsPerRender,             TEXT("CA_IncreaseStepsPerRender"),             EKeys::T },
			{ EHotkey::DecreaseStepsPerRender,             TEXT("CA_DecreaseStepsPerRender"),             EKeys::G },
			{ EHotkey::MoveCullVolumeUp,                   TEXT("CA_MoveCullVolumeUp"),                   EKeys::Up },
			{ EHotkey::MoveCullVolumeDown,                 TEXT("CA_MoveCullVolumeDown"),                 EKeys::Down },
			{ EHotkey::MoveCullVolumeLeft,                 TEXT("CA_MoveCullVolumeLeft"),                 EKeys::Left },
			{ EHotkey::MoveCullVolumeRight,                TEXT("CA_MoveCullVolumeRight"),                EKeys::Right },
			{ EHotkey::ToggleViewSlice,                    TEXT("CA_ToggleViewSlice"),                    EKeys::J },
			{ EHotkey::ViewSliceNearer,                    TEXT("CA_ViewSliceNearer"),                    EKeys::LeftBracket },
			{ EHotkey::ViewSliceFarther,                   TEXT("CA_ViewSliceFarther"),                   EKeys::RightBracket },
			{ EHotkey::ToggleSelectionMode,                TEXT("CA_ToggleSelectionMode"),                EKeys::Tab },
			{ EHotkey::SelectDrag,                         TEXT("CA_SelectDrag"),                         EKeys::LeftMouseButton },
			{ EHotkey::ExtractSelection,                   TEXT("CA_ExtractSelection"),                   EKeys::Enter },
			{ EHotkey::InvertSelection,                    TEXT("CA_InvertSelection"),                    EKeys::I },
			{ EHotkey::BakeCellsToMesh,                    TEXT("CA_BakeCellsToMesh"),                    EKeys::M },
			{ EHotkey::DeleteSelectedCells,                TEXT("CA_DeleteSelectedCells"),                EKeys::Delete },
			{ EHotkey::MoveCullVolumeToSelection,          TEXT("CA_MoveCullVolumeToSelection"),          EKeys::K },
			{ EHotkey::SelectCellsInCullVolume,            TEXT("CA_SelectCellsInCullVolume"),            EKeys::L },
			{ EHotkey::SaveState,                          TEXT("CA_SaveState"),                          EKeys::S },
			{ EHotkey::LoadState,                          TEXT("CA_LoadState"),                          EKeys::O },

			// --- Через сырой InputKey() ---
			{ EHotkey::ToggleSimulation,                   TEXT("CA_ToggleSimulation"),                   EKeys::SpaceBar },
			{ EHotkey::ResetSimulation,                    TEXT("CA_ResetSimulation"),                    EKeys::R },
			{ EHotkey::NewSeed,                            TEXT("CA_NewSeed"),                            EKeys::N },
			{ EHotkey::GenerateState,                      TEXT("CA_GenerateState"),                      EKeys::Y },
			// Делит Z с ToggleChunkedRender намеренно: отмена требует Ctrl, а
			// голая Z уходит чанковому рендеру, который сам отсеивает нажатие с
			// модификатором (выразить "без модификатора" в привязке нельзя).
			{ EHotkey::UndoRedo,                           TEXT("CA_UndoRedo"),                           EKeys::Z, /*bModifierGuarded=*/true },
			{ EHotkey::ToggleHudInfoPanel,                 TEXT("CA_ToggleHudInfoPanel"),                 EKeys::F5 },
			{ EHotkey::TakePhotoShot,                      TEXT("CA_TakePhotoShot"),                      EKeys::F10 },
			{ EHotkey::ToggleSonification,                 TEXT("CA_ToggleSonification"),                 EKeys::P },
			{ EHotkey::CaptureTextureSlice,                TEXT("CA_CaptureTextureSlice"),                EKeys::F6 },
			{ EHotkey::ToggleSeriesCapture,                TEXT("CA_ToggleSeriesCapture"),                EKeys::F7 },

			// Клавиша СДВИНУТА на единицу относительно возраста: 1 показывает
			// возраст 0 (только что родившиеся), 9 - возраст 8, а 0 достаётся
			// возрасту 9 вместе со всем, что старше.
			//
			// Так цифровой ряд читается слева направо ровно как рампа возрастов:
			// 1,2,...,9,0 - это возрасты 0,1,...,8 и хвост. Раньше клавиша
			// совпадала с возрастом (0 -> 0), и хвост сидел на 9 посреди ряда,
			// а крайняя правая клавиша означала середину рампы.
			//
			// Имена действий остались по ВОЗРАСТУ, а не по клавише
			// (CA_AgeFilter0 - это возраст 0, где бы он ни лежал): InputKey()
			// достаёт их арифметикой AgeFilter0 + Age, и переименование по
			// клавише сделало бы эту связь ложной при первом же переназначении.
			{ EHotkey::AgeFilter0,                         TEXT("CA_AgeFilter0"),                         EKeys::One },
			{ EHotkey::AgeFilter1,                         TEXT("CA_AgeFilter1"),                         EKeys::Two },
			{ EHotkey::AgeFilter2,                         TEXT("CA_AgeFilter2"),                         EKeys::Three },
			{ EHotkey::AgeFilter3,                         TEXT("CA_AgeFilter3"),                         EKeys::Four },
			{ EHotkey::AgeFilter4,                         TEXT("CA_AgeFilter4"),                         EKeys::Five },
			{ EHotkey::AgeFilter5,                         TEXT("CA_AgeFilter5"),                         EKeys::Six },
			{ EHotkey::AgeFilter6,                         TEXT("CA_AgeFilter6"),                         EKeys::Seven },
			{ EHotkey::AgeFilter7,                         TEXT("CA_AgeFilter7"),                         EKeys::Eight },
			{ EHotkey::AgeFilter8,                         TEXT("CA_AgeFilter8"),                         EKeys::Nine },
			{ EHotkey::AgeFilter9,                         TEXT("CA_AgeFilter9"),                         EKeys::Zero },

			{ EHotkey::ToggleOrthographic,                 TEXT("CA_ToggleOrthographic"),                 EKeys::NumPadFive },
			{ EHotkey::FrameAllCellsFromNumPad,            TEXT("CA_FrameAllCellsNumPad"),                EKeys::NumPadZero },
			{ EHotkey::AlignCameraToOppositeSide,          TEXT("CA_AlignCameraToOppositeSide"),          EKeys::NumPadNine },
			{ EHotkey::FrameSelection,                     TEXT("CA_FrameSelection"),                     EKeys::Decimal },
			{ EHotkey::OrthoZoomIn,                        TEXT("CA_OrthoZoomIn"),                        EKeys::Multiply },
			{ EHotkey::OrthoZoomOut,                       TEXT("CA_OrthoZoomOut"),                       EKeys::Divide },
			{ EHotkey::ViewLeft,                           TEXT("CA_ViewLeft"),                           EKeys::NumPadFour },
			{ EHotkey::ViewRight,                          TEXT("CA_ViewRight"),                          EKeys::NumPadSix },
			{ EHotkey::ViewTop,                            TEXT("CA_ViewTop"),                            EKeys::NumPadEight },
			{ EHotkey::ViewBottom,                         TEXT("CA_ViewBottom"),                         EKeys::NumPadTwo },
			{ EHotkey::ViewFront,                          TEXT("CA_ViewFront"),                          EKeys::NumPadOne },
			{ EHotkey::ViewBack,                           TEXT("CA_ViewBack"),                           EKeys::NumPadThree },
			{ EHotkey::ViewIsometric,                      TEXT("CA_ViewIsometric"),                      EKeys::NumPadSeven },
		};

		return Defaults;
	}

	FName GetActionName(EHotkey Hotkey)
	{
		const TArray<FHotkeyDefault>& Defaults = GetDefaults();
		const int32 Index = (int32)Hotkey;
		if (!Defaults.IsValidIndex(Index))
		{
			return NAME_None;
		}
		return FName(Defaults[Index].ActionName);
	}

	TArray<FKey> ResolveKeys()
	{
		const TArray<FHotkeyDefault>& Defaults = GetDefaults();

		TArray<FKey> Resolved;
		Resolved.Reserve(Defaults.Num());
		for (const FHotkeyDefault& Default : Defaults)
		{
			Resolved.Add(Default.DefaultKey);
		}

		const UInputSettings* Settings = GetDefault<UInputSettings>();
		if (!Settings)
		{
			return Resolved;
		}

		// Один проход по маппингам, а не поиск по имени на каждую клавишу: их
		// под сотню с каждой стороны, и квадрат тут не нужен ни за чем.
		TMap<FName, FKey> Configured;
		for (const FInputActionKeyMapping& Mapping : Settings->GetActionMappings())
		{
			// Первое вхождение выигрывает: ini позволяет несколько клавиш на
			// одно имя, а здесь у действия клавиша ровно одна (две клавиши на
			// одно действие выражаются двумя строками таблицы - см. скорость).
			if (!Configured.Contains(Mapping.ActionName))
			{
				Configured.Add(Mapping.ActionName, Mapping.Key);
			}
		}

		for (int32 Index = 0; Index < Defaults.Num(); ++Index)
		{
			const FName ActionName(Defaults[Index].ActionName);
			if (const FKey* ConfiguredKey = Configured.Find(ActionName))
			{
				// Невалидная клавиша в ini (опечатка в имени клавиши) даёт
				// EKeys::Invalid, а не отказ разбора - без этой проверки хоткей
				// молча перестал бы работать вовсе.
				if (ConfiguredKey->IsValid())
				{
					Resolved[Index] = *ConfiguredKey;
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("Хоткеи: у действия %s в конфиге неизвестная клавиша - остаётся значение по умолчанию (%s)"),
						*ActionName.ToString(), *Defaults[Index].DefaultKey.ToString());
				}
			}
		}

		return Resolved;
	}

	TMap<FKey, TArray<FName>> FindConflicts(const TArray<FKey>& ResolvedKeys)
	{
		const TArray<FHotkeyDefault>& Defaults = GetDefaults();

		TMap<FKey, TArray<FName>> ByKey;
		for (int32 Index = 0; Index < ResolvedKeys.Num() && Index < Defaults.Num(); ++Index)
		{
			if (Defaults[Index].bModifierGuarded)
			{
				continue;
			}
			ByKey.FindOrAdd(ResolvedKeys[Index]).Add(FName(Defaults[Index].ActionName));
		}

		// Остаются только те, где действий больше одного.
		for (auto It = ByKey.CreateIterator(); It; ++It)
		{
			if (It.Value().Num() < 2)
			{
				It.RemoveCurrent();
			}
		}

		return ByKey;
	}
}
