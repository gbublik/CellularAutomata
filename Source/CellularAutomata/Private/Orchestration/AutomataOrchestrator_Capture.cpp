// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Core/PlayerController/GamePlayerController.h"
#include "Automata/Capture/CellRasterizer.h"
#include "Automata/Capture/CapturePresets.h"
#include "Automata/Generation/StateGenerators.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"


FString AAutomataOrchestrator::EnsureSliceDirectory() const
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AutomataSlices"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	return Dir;
}

bool AAutomataOrchestrator::BuildSliceCapture(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
{
	if (!Grid || Grid->Num() == 0)
	{
		OutError = TEXT("сетка пуста - снимать нечего");
		return false;
	}

	// Тот же сбор, что готовит экранный рендер: клетки уже отфильтрованы кубом,
	// возрастом и срезом, и уже окрашены по рампе. Побочный эффект - перезапись
	// LastRenderStats, поэтому сохраняем и возвращаем: съёмка не должна
	// подменять собой статистику последнего РЕНДЕРА, которую показывает HUD.
	const FCellRenderStats SavedRenderStats = LastRenderStats;

	// Цвета для файла нужны гамма-кодированными, в отличие от экранных - см.
	// FSliceCaptureParams::bEncodeSRGB. Признак снимается на время сбора.
	const bool bSavedCaptureColors = bBuildingSliceCapture;
	bBuildingSliceCapture = SliceCaptureParams.bEncodeSRGB;

	TArray<FCellRenderInstance> Cells;
	BuildCellRenderData(Cells);

	bBuildingSliceCapture = bSavedCaptureColors;
	LastRenderStats = SavedRenderStats;

	if (Cells.Num() == 0)
	{
		OutError = TEXT("после фильтров не осталось ни одной видимой клетки");
		return false;
	}

	CellRasterizer::FRasterParams RasterParams;
	RasterParams.CellWorldStep = Grid->GetLattice().GetCellWorldExtent();
	RasterParams.PixelsPerCell = FMath::Max(SliceCaptureParams.PixelsPerCell, 1);
	RasterParams.Mode = SliceCaptureParams.Mode;
	RasterParams.ForegroundColor = SliceCaptureParams.ForegroundColor;
	RasterParams.BackgroundColor = SliceCaptureParams.bTransparentBackground
		? FColor(0, 0, 0, 0)
		: SliceCaptureParams.BackgroundColor;

	// Оси - от камеры, но только как ориентир "с какой стороны": базис
	// округляется до осевого, иначе клетки не легли бы в регулярную сетку.
	FVector CameraForward = FVector::ForwardVector;
	FVector CameraUp = FVector::UpVector;
	if (GamePC && GamePC->PlayerCameraManager)
	{
		const FRotationMatrix CameraBasis(GamePC->PlayerCameraManager->GetCameraRotation());
		CameraForward = CameraBasis.GetUnitAxis(EAxis::X);
		// Верх берётся У КАМЕРЫ, а не из мира: при взгляде сверху вниз мировой
		// "вверх" вырожден, а камерный указывает, где у картинки север.
		CameraUp = CameraBasis.GetUnitAxis(EAxis::Z);
	}

	CellRasterizer::BuildAxes(CameraForward, CameraUp, RasterParams);

	CellRasterizer::FRasterImage Image;
	if (!CellRasterizer::Rasterize(Cells, RasterParams, MaxCapturePixels, Image, OutError))
	{
		return false;
	}

	// Отражение - постобработка над готовым снимком, а не отдельный путь
	// растеризации: тайл это то же изображение плюс его зеркала, и знать про
	// клетки для этого не нужно.
	if (SliceCaptureParams.TileMode != ESliceTileMode::None)
	{
		const bool bMirrorX = (SliceCaptureParams.TileMode == ESliceTileMode::MirrorX || SliceCaptureParams.TileMode == ESliceTileMode::MirrorBoth);
		const bool bMirrorY = (SliceCaptureParams.TileMode == ESliceTileMode::MirrorY || SliceCaptureParams.TileMode == ESliceTileMode::MirrorBoth);
		CellRasterizer::MakeTile(Image, bMirrorX, bMirrorY);
	}

	OutWidth = Image.Width;
	OutHeight = Image.Height;
	OutPixels = MoveTemp(Image.Pixels);
	return true;
}

bool AAutomataOrchestrator::EstimateSliceCaptureSize(int32& OutWidth, int32& OutHeight)
{
	if (!Grid || Grid->Num() == 0)
	{
		return false;
	}

	const FCellRenderStats SavedRenderStats = LastRenderStats;
	TArray<FCellRenderInstance> Cells;
	BuildCellRenderData(Cells);
	LastRenderStats = SavedRenderStats;

	CellRasterizer::FRasterParams RasterParams;
	RasterParams.CellWorldStep = Grid->GetLattice().GetCellWorldExtent();
	RasterParams.PixelsPerCell = FMath::Max(SliceCaptureParams.PixelsPerCell, 1);

	FVector CameraForward = FVector::ForwardVector;
	FVector CameraUp = FVector::UpVector;
	if (GamePC && GamePC->PlayerCameraManager)
	{
		const FRotationMatrix CameraBasis(GamePC->PlayerCameraManager->GetCameraRotation());
		CameraForward = CameraBasis.GetUnitAxis(EAxis::X);
		CameraUp = CameraBasis.GetUnitAxis(EAxis::Z);
	}

	CellRasterizer::BuildAxes(CameraForward, CameraUp, RasterParams);
	return CellRasterizer::ComputeImageSize(Cells, RasterParams, OutWidth, OutHeight);
}

bool AAutomataOrchestrator::WriteSliceCaptureToFile(const FString& FilePath)
{
	const double StartSeconds = FPlatformTime::Seconds();

	TArray<FColor> Pixels;
	int32 Width = 0;
	int32 Height = 0;
	FString Error;

	if (!BuildSliceCapture(Pixels, Width, Height, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureTextureSlice: %s"), *Error);
		ShowStatusMessage(StatusKey_SliceCapture, FString::Printf(TEXT("Снимок не сделан: %s"), *Error));
		return false;
	}

	TArray64<uint8> PngBytes;
	FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), PngBytes);

	if (PngBytes.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureTextureSlice: не удалось закодировать PNG"));
		ShowStatusMessage(StatusKey_SliceCapture, TEXT("Снимок не сделан: ошибка кодирования PNG"));
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(PngBytes, *FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureTextureSlice: не удалось записать файл '%s'"), *FilePath);
		ShowStatusMessage(StatusKey_SliceCapture, TEXT("Снимок не сделан: файл не записан"));
		return false;
	}

	const double Seconds = FPlatformTime::Seconds() - StartSeconds;
	const double MegaBytes = static_cast<double>(PngBytes.Num()) / (1024.0 * 1024.0);

	UE_LOG(LogTemp, Log, TEXT("CaptureTextureSlice: %dx%d, %.2f МБ, %.2f мс -> %s"),
		Width, Height, MegaBytes, Seconds * 1000.0, *FilePath);
	ShowStatusMessage(StatusKey_SliceCapture,
		FString::Printf(TEXT("Снимок %dx%d (%.1f МБ): %s"), Width, Height, MegaBytes, *FPaths::GetCleanFilename(FilePath)));

	return true;
}

void AAutomataOrchestrator::CaptureTextureSlice()
{
	// Без диалога намеренно: снимки делают сериями, и ценность в том, что
	// нажатие ничего не спрашивает. Полный путь уходит в лог.
	const FString FileName = FString::Printf(TEXT("Slice_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	WriteSliceCaptureToFile(EnsureSliceDirectory() / FileName);
}

TArray<FCapturePreset> AAutomataOrchestrator::GetCapturePresets() const
{
	return CapturePresets::GetAll();
}

FString AAutomataOrchestrator::GetActiveCapturePresetName() const
{
	const TArray<FCapturePreset>& Presets = CapturePresets::GetAll();
	return Presets.IsValidIndex(ActiveCapturePresetIndex) ? Presets[ActiveCapturePresetIndex].Name : FString();
}

void AAutomataOrchestrator::ApplyCapturePreset(int32 PresetIndex)
{
	const TArray<FCapturePreset>& Presets = CapturePresets::GetAll();
	if (!Presets.IsValidIndex(PresetIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyCapturePreset: индекс %d вне диапазона (наборов: %d)"), PresetIndex, Presets.Num());
		return;
	}

	const FCapturePreset& Preset = Presets[PresetIndex];

	// Присваивание целиком, а не поле за полем: в этом и смысл того, что пресет
	// хранит всю структуру - хвостов от предыдущего набора не остаётся по
	// построению, а не потому, что кто-то не забыл их сбросить.
	SliceCaptureParams = Preset.Params;
	ActiveCapturePresetIndex = PresetIndex;

	// Идущую серию НЕ трогаем: её длина и шаг зафиксированы на старте
	// (SeriesFramesRemaining), а размер кадра поменялся бы прямо посреди неё -
	// получилась бы серия из кадров разного размера, непригодная ни для
	// анимации, ни для перебора.
	if (bSeriesCaptureActive)
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyCapturePreset: серия ещё идёт - новый набор вступит в силу со следующего запуска F7"));
	}

	UE_LOG(LogTemp, Log, TEXT("ApplyCapturePreset: '%s' (режим %d, плитка %d, %d пикс/клетка, серия %d x %d поколений)"),
		*Preset.Name,
		static_cast<int32>(SliceCaptureParams.Mode),
		static_cast<int32>(SliceCaptureParams.TileMode),
		SliceCaptureParams.PixelsPerCell,
		SliceCaptureParams.SeriesFrameCount,
		SliceCaptureParams.SeriesGenerationsPerFrame);

	ShowStatusMessage(StatusKey_SliceCapture,
		FString::Printf(TEXT("Съёмка: %s - %s"), *Preset.Name, *Preset.Description));
}

void AAutomataOrchestrator::CycleCapturePreset()
{
	const TArray<FCapturePreset>& Presets = CapturePresets::GetAll();
	if (Presets.Num() == 0)
	{
		return;
	}

	// INDEX_NONE (ни одного набора ещё не применяли) даёт 0 - первый набор, а не
	// второй: настройки правили руками, и логичный первый шаг перебора - начало
	// списка.
	const int32 NextIndex = Presets.IsValidIndex(ActiveCapturePresetIndex)
		? (ActiveCapturePresetIndex + 1) % Presets.Num()
		: 0;

	ApplyCapturePreset(NextIndex);
}

void AAutomataOrchestrator::WriteSeriesManifest()
{
	if (SeriesDirectory.IsEmpty())
	{
		return;
	}

	const int32 PerFrame = FMath::Max(SliceCaptureParams.SeriesGenerationsPerFrame, 1);
	// Поколение, на котором серия начинается. Кадр N снят на
	// StartGeneration + N * PerFrame - это и есть та единственная координата,
	// которой не хватает в самом PNG.
	const int64 StartGeneration = GenerationCount;

	// .casave - тем же путём, что Ctrl+S, вплоть до миниатюры: отдельный
	// формат "почти как сохранение" разъехался бы с настоящим при первой же
	// правке шапки. LastSaveFilePath при этом не трогаем.
	const FString SavePath = SeriesDirectory / TEXT("Series.casave");
	const bool bSaved = WriteStateToFile(SavePath, /*bUpdateLastSavePath=*/false);
	if (!bSaved)
	{
		// Причина уже в логе. Серию не прерываем - кадры важнее паспорта.
		UE_LOG(LogTemp, Warning, TEXT("WriteSeriesManifest: состояние не сохранено, кадры серии останутся без точки возврата"));
	}

	// Текстовая записка рядом с бинарником: .casave не прочитать в Проводнике,
	// а понять, что за папка, нужно бывает без запуска редактора вовсе.
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Серия снимков клеточного автомата")));
	Lines.Add(FString::Printf(TEXT("Снято: %s"), *FDateTime::Now().ToString()));
	Lines.Add(TEXT(""));
	Lines.Add(FString::Printf(TEXT("Правило:              %s"), *GetActiveRuleString()));
	Lines.Add(FString::Printf(TEXT("Seed:                 %d"), Seed));
	Lines.Add(FString::Printf(TEXT("Генератор: %s, Radius / Amount: %d / %d"),
		*StateGenerators::GetDisplayName(GenerationParams.Type), GenerationParams.Radius, GenerationParams.Amount));
	Lines.Add(FString::Printf(TEXT("CellSize / ChunkSize: %.1f / %d"), CellSize, ChunkSize));
	Lines.Add(TEXT(""));
	Lines.Add(FString::Printf(TEXT("Поколение на старте:  %lld"), StartGeneration));
	Lines.Add(FString::Printf(TEXT("Поколений на кадр:    %d"), PerFrame));
	Lines.Add(FString::Printf(TEXT("Кадров запрошено:     %d"), SeriesFramesRemaining));
	Lines.Add(TEXT(""));
	Lines.Add(FString::Printf(TEXT("Набор съёмки:         %s"), *GetActiveCapturePresetName()));
	Lines.Add(FString::Printf(TEXT("Режим растеризации:   %d (0=NearestToCamera, 1=Silhouette, 2=Thickness)"),
		static_cast<int32>(SliceCaptureParams.Mode)));
	Lines.Add(FString::Printf(TEXT("Плитка:               %d (0=None, 1=MirrorX, 2=MirrorY, 3=MirrorBoth)"),
		static_cast<int32>(SliceCaptureParams.TileMode)));
	Lines.Add(FString::Printf(TEXT("Пикселей на клетку:   %d"), SliceCaptureParams.PixelsPerCell));
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("Как переснять кадр Frame_NNNN крупнее или в другом цвете:"));
	Lines.Add(TEXT("  1. Ctrl+O -> Series.casave из этой папки."));
	Lines.Add(FString::Printf(TEXT("  2. Выставить StepsPerRender = %d (клавиши T/G)."), PerFrame));
	Lines.Add(TEXT("  3. Нажать F ровно NNNN раз (счётчик поколений в HUD - ориентир)."));
	Lines.Add(TEXT("  4. Shift+F7 до нужного набора, затем F6."));
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("Цвет пересъёмке не мешает: снимок растеризуется из сетки, поэтому"));
	Lines.Add(TEXT("то же правило + тот же сид + то же поколение дают тот же кадр"));
	Lines.Add(TEXT("бит в бит при любой палитре AgeColors."));

	const FString ManifestPath = SeriesDirectory / TEXT("Series.txt");
	// ForceUTF8 - это UTF-8 ИМЕННО с BOM (без него - отдельное значение
	// ForceUTF8WithoutBOM). BOM здесь обязателен: без него Блокнот прочитает
	// кириллицу как мусор.
	if (!FFileHelper::SaveStringArrayToFile(Lines, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8))
	{
		UE_LOG(LogTemp, Warning, TEXT("WriteSeriesManifest: не удалось записать %s"), *ManifestPath);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("WriteSeriesManifest: паспорт серии записан (%s), поколение на старте %lld, %d поколений на кадр"),
		bSaved ? TEXT("Series.casave + Series.txt") : TEXT("только Series.txt"), StartGeneration, PerFrame);
}

void AAutomataOrchestrator::StartSeriesCapture()
{
	if (bSeriesCaptureActive)
	{
		// Повторный вызов - обрыв: у хоткея одна клавиша на оба действия, и
		// прервать серию должно быть так же просто, как начать.
		StopSeriesCapture();
		return;
	}

	if (!Grid || Grid->Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartSeriesCapture: сетка пуста - снимать нечего"));
		ShowStatusMessage(StatusKey_SliceCapture, TEXT("Серия не начата: сетка пуста"));
		return;
	}

	// Своя подпапка на запуск: кадры одной серии должны лежать вместе и
	// нумероваться подряд, иначе их не собрать в анимацию.
	SeriesDirectory = EnsureSliceDirectory() / FString::Printf(TEXT("Series_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	IFileManager::Get().MakeDirectory(*SeriesDirectory, /*Tree=*/true);

	SeriesFrameIndex = 0;
	SeriesFramesRemaining = FMath::Max(SliceCaptureParams.SeriesFrameCount, 1);
	SeriesGenerationsSinceFrame = 0;
	bSeriesCaptureActive = true;

	UE_LOG(LogTemp, Log, TEXT("StartSeriesCapture: %d кадров через каждые %d поколений -> %s"),
		SeriesFramesRemaining, FMath::Max(SliceCaptureParams.SeriesGenerationsPerFrame, 1), *SeriesDirectory);

	// Паспорт серии пишется ДО первого кадра: даже оборванная на первом же
	// снимке серия оставляет папку, по которой понятно, что это было.
	WriteSeriesManifest();

	// Текущее состояние - это тоже кадр серии, и притом единственный, который
	// пользователь видел глазами, когда решил снимать.
	CaptureSeriesFrame();

	// Серия могла закончиться на первом же кадре (SeriesFrameCount == 1).
	if (!bSeriesCaptureActive)
	{
		return;
	}

	if (!bSimulationRunning && !IsFastStepActive())
	{
		bSeriesStartedSimulation = true;
		Start();
	}
}

void AAutomataOrchestrator::StopSeriesCapture()
{
	if (!bSeriesCaptureActive)
	{
		return;
	}

	const int32 Captured = SeriesFrameIndex;
	bSeriesCaptureActive = false;
	SeriesFramesRemaining = 0;
	SeriesGenerationsSinceFrame = 0;

	// Останавливаем только то, что сами и запустили.
	if (bSeriesStartedSimulation)
	{
		bSeriesStartedSimulation = false;
		if (bSimulationRunning)
		{
			Stop();
		}
	}

	// Догоняющего рендера здесь нет намеренно. В быстром режиме промежуточные
	// поколения на экран не выводились, и на нём осталась картинка того
	// состояния, с которого серию запустили (F7) - именно она и должна
	// остаться. Финальное поколение уже лежит в последнем PNG, а рендер
	// многомиллионной сетки - самая дорогая операция кадра, и платить за неё
	// ради картинки, которую всё равно смотрят в файлах, незачем. Экран
	// обновится сам при следующем рендере (P, F, R, N).

	UE_LOG(LogTemp, Log, TEXT("StopSeriesCapture: снято %d кадров -> %s"), Captured, *SeriesDirectory);
	ShowStatusMessage(StatusKey_SliceCapture,
		FString::Printf(TEXT("Серия завершена: %d кадров в %s"), Captured, *FPaths::GetCleanFilename(SeriesDirectory)));
}

void AAutomataOrchestrator::CaptureSeriesFrame()
{
	const FString FileName = FString::Printf(TEXT("Frame_%04d.png"), SeriesFrameIndex);

	if (!WriteSliceCaptureToFile(SeriesDirectory / FileName))
	{
		// Причину уже сказал WriteSliceCaptureToFile(). Серию обрываем: если
		// кадр не снялся, следующие почти наверняка не снимутся тоже, а
		// молча продолжать капать ошибками в лог - худший вариант.
		UE_LOG(LogTemp, Warning, TEXT("CaptureSeriesFrame: кадр %d не снят - серия прервана"), SeriesFrameIndex);
		StopSeriesCapture();
		return;
	}

	++SeriesFrameIndex;
	--SeriesFramesRemaining;

	if (SeriesFramesRemaining <= 0)
	{
		StopSeriesCapture();
		return;
	}

	// Своё сообщение поверх того, что показал WriteSliceCaptureToFile(): в
	// серии важнее прогресс, чем размер очередного файла.
	ShowStatusMessage(StatusKey_SliceCapture,
		FString::Printf(TEXT("Серия: кадр %d из %d"), SeriesFrameIndex, SeriesFrameIndex + SeriesFramesRemaining));
}

void AAutomataOrchestrator::CaptureTextureSliceAs()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureTextureSliceAs: диалоги недоступны - снимок не сделан"));
		return;
	}

	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	const FString DefaultFileName = FString::Printf(TEXT("Slice_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

	TArray<FString> PickedFiles;
	const bool bPicked = DesktopPlatform->SaveFileDialog(
		ParentWindowHandle,
		TEXT("Сохранить срез"),
		EnsureSliceDirectory(),
		DefaultFileName,
		TEXT("PNG Image (*.png)|*.png"),
		EFileDialogFlags::None,
		PickedFiles);

	if (!bPicked || PickedFiles.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("CaptureTextureSliceAs: отменено"));
		return;
	}

	FString FilePath = FPaths::ConvertRelativePathToFull(PickedFiles[0]);
	if (FPaths::GetExtension(FilePath).IsEmpty())
	{
		FilePath += TEXT(".png");
	}

	WriteSliceCaptureToFile(FilePath);
}
