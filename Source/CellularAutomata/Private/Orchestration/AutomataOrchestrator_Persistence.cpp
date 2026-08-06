// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Automata/Persistence/AutomatonStateSerializer.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "UnrealClient.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"


FString AAutomataOrchestrator::EnsureSaveDirectory() const
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AutomataSaves"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	return Dir;
}

FAutomatonSaveHeader AAutomataOrchestrator::BuildSaveHeader() const
{
	FAutomatonSaveHeader Header;
	Header.BirthCounts = BirthCounts;
	Header.SurvivalCounts = SurvivalCounts;
	Header.Neighborhood = Neighborhood;
	Header.States = States;
	// CellSize - из сетки, не из UPROPERTY: сетка могла быть создана со
	// старым значением, а файл фиксирует её фактическую геометрию.
	Header.CellSize = Grid ? Grid->GetCellSize() : CellSize;
	// Растяжение по Z - тоже из сетки и по той же причине. Берётся отношением
	// шагов, а не копированием UPROPERTY: в сетке лежит фактическая геометрия,
	// с которой клетки и расставлены.
	Header.LatticeZScale = Grid
		? static_cast<float>(Grid->GetLattice().GetCellWorldExtent().Z / FMath::Max(Grid->GetLattice().GetCellWorldExtent().X, UE_DOUBLE_SMALL_NUMBER))
		: LatticeZScale;
	// Фильтр чётности - часть геометрии не меньше, чем шаг: без него первое же
	// пересевание после загрузки (N/Y) уйдёт на другую подрешётку.
	Header.ParityFilter = GenerationParams.ParityFilter;
	Header.NeighborhoodShape = NeighborhoodShape;
	Header.ChunkSize = ChunkSize;
	Header.GridSize = GridSize;
	Header.Seed = Seed;
	// Amount/SpawnRadius пишутся из параметров ГЕНЕРАТОРА - отдельного блока
	// Automata|Random больше нет. Поля в заголовке оставлены как есть, чтобы
	// файлы читались и старой сборкой: для неё это по-прежнему радиус и число
	// клеток случайного шара, а смысл совпадает, когда выбран RandomBall.
	// ClusterFactor не пишется вовсе - он не влиял на генерацию уже давно
	// (GenerateRandom() передавал в генератор только радиус и количество), так
	// что в заголовке остаётся его значение по умолчанию.
	Header.Amount = GenerationParams.Amount;
	Header.SpawnRadius = GenerationParams.Radius;
	// Header.CellCount выставит WriteSave() из фактического набора клеток.
	return Header;
}

void AAutomataOrchestrator::ApplySaveHeader(const FAutomatonSaveHeader& Header)
{
	// JSON-шапка правится руками в текстовом редакторе - значениям нельзя
	// доверять, клампы повторяют ClampMin соответствующих UPROPERTY.
	BirthCounts = Header.BirthCounts;
	SurvivalCounts = Header.SurvivalCounts;
	Neighborhood = Header.Neighborhood;
	// Миграция: пока существовал отдельный радиус, нынешний VonNeumann2
	// записывался как VonNeumann с NeighborhoodRadius=2. Без этой строки такой
	// файл загрузился бы как VonNeumann - молча, с 6 соседями вместо 24 и
	// совсем другой картинкой. Поле в шапке оставлено только ради этой
	// проверки (см. FAutomatonSaveHeader::NeighborhoodRadius).
	if (Header.NeighborhoodRadius == 2 && Header.Neighborhood == ENeighborhood::VonNeumann)
	{
		Neighborhood = ENeighborhood::VonNeumann2;
	}
	States = FMath::Max(2, Header.States);
	CellSize = FMath::Max(1.0f, Header.CellSize);
	// Кламп повторяет ClampMin/UIMax самого UPROPERTY: шапка правится руками,
	// а нулевое или отрицательное растяжение схлопнуло бы решётку в плоскость.
	LatticeZScale = FMath::Clamp(Header.LatticeZScale, 0.1f, 10.0f);
	GenerationParams.ParityFilter = Header.ParityFilter;
	NeighborhoodShape = Header.NeighborhoodShape;
	ChunkSize = FMath::Max(1, Header.ChunkSize);
	GridSize.X = FMath::Max(1, Header.GridSize.X);
	GridSize.Y = FMath::Max(1, Header.GridSize.Y);
	GridSize.Z = FMath::Max(1, Header.GridSize.Z);
	Seed = Header.Seed;
	// Едут в параметры генератора. Тип при этом НЕ трогается: файл хранит
	// начальный набор клеток целиком (InitialCells), поэтому что именно им
	// когда-то построили, значения не имеет, а сбрасывать выбранный
	// пользователем генератор при загрузке было бы неожиданно.
	GenerationParams.Amount = FMath::Max(1, Header.Amount);
	GenerationParams.Radius = FMath::Max(1, Header.SpawnRadius);
	// Header.ClusterFactor намеренно игнорируется - см. BuildSaveHeader().
}

bool AAutomataOrchestrator::CaptureThumbnailPng(TArray64<uint8>& OutPngBytes) const
{
	OutPngBytes.Reset();

	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureThumbnailPng: GameViewport недоступен (не PIE?) - миниатюра пропущена"));
		return false;
	}

	FViewport* Viewport = GEngine->GameViewport->Viewport;
	const FIntPoint ViewportSize = Viewport->GetSizeXY();
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureThumbnailPng: нулевой размер вьюпорта - миниатюра пропущена"));
		return false;
	}

	TArray<FColor> RawPixels;
	if (!Viewport->ReadPixels(RawPixels) || RawPixels.Num() != ViewportSize.X * ViewportSize.Y)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureThumbnailPng: ReadPixels не удался - миниатюра пропущена"));
		return false;
	}

	// Альфа бэкбуфера не несёт полезного смысла для миниатюры (часто 0) -
	// принудительно делаем непрозрачным, иначе PNG вышел бы прозрачным.
	for (FColor& Pixel : RawPixels)
	{
		Pixel.A = 255;
	}

	// Обрезаем до квадрата ПО ЦЕНТРУ: короткая сторона вьюпорта берётся
	// целиком, длинная обрезается симметрично по краям - никакого искажения
	// пропорций, просто теряются края кадра. Строка за строкой memcpy прямо
	// из RawPixels, отдельный проход resize не нужен для самой обрезки.
	const int32 CropSize = FMath::Min(ViewportSize.X, ViewportSize.Y);
	const int32 CropOffsetX = (ViewportSize.X - CropSize) / 2;
	const int32 CropOffsetY = (ViewportSize.Y - CropSize) / 2;

	TArray<FColor> CroppedPixels;
	CroppedPixels.SetNumUninitialized(CropSize * CropSize);
	for (int32 Row = 0; Row < CropSize; ++Row)
	{
		const FColor* SrcRow = RawPixels.GetData() + (CropOffsetY + Row) * ViewportSize.X + CropOffsetX;
		FColor* DstRow = CroppedPixels.GetData() + Row * CropSize;
		FMemory::Memcpy(DstRow, SrcRow, CropSize * sizeof(FColor));
	}

	// Безусловное масштабирование (не только "если больше") - сторона
	// квадрата всегда становится РОВНО ThumbnailSizePixels, независимо от
	// текущего разрешения вьюпорта: единый стандартный размер миниатюры.
	TArray<FColor> ResizedPixels;
	const TArray<FColor>* PixelsToEncode = &CroppedPixels;
	int32 EncodeSize = CropSize;

	if (CropSize != ThumbnailSizePixels)
	{
		EncodeSize = FMath::Max(1, ThumbnailSizePixels);
		FImageUtils::ImageResize(CropSize, CropSize, CroppedPixels, EncodeSize, EncodeSize,
			ResizedPixels, /*bResizeSRGBinLinearSpace=*/true, /*bForceOpaqueOutput=*/true);
		PixelsToEncode = &ResizedPixels;
	}

	FImageUtils::PNGCompressImageArray(EncodeSize, EncodeSize,
		TArrayView64<const FColor>(PixelsToEncode->GetData(), PixelsToEncode->Num()), OutPngBytes);

	if (OutPngBytes.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureThumbnailPng: PNG-кодирование дало пустой результат - миниатюра пропущена"));
		return false;
	}

	return true;
}

bool AAutomataOrchestrator::WriteStateToFile(const FString& FilePath, bool bUpdateLastSavePath)
{
	// Сохраняем ИЗНАЧАЛЬНЫЙ паттерн (InitialStateCells) - НЕ трогая Grid:
	// ни сброса, ни перерисовки, ни движения камеры. InitialStateCells
	// заполняется либо StartFromSelection() (Enter), либо
	// LoadStateFromFile() - обе строго на game thread, так что читать этот
	// массив здесь безопасно без bStepInProgress guard'а (в отличие от
	// LoadStateFromFile(), эта функция не свапает Grid вовсе).
	if (InitialStateCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("WriteStateToFile: нет изначального паттерна для сохранения - сначала извлеките выделение через Enter или загрузите файл"));
		return false;
	}

	// Миниатюра - скриншот ТЕКУЩЕГО вида (какая сейчас камера, какая сейчас
	// живая симуляция на экране), а не сохраняемого паттерна - см.
	// doc-comment в заголовке. Косметика: неудача не прерывает сохранение
	// (CaptureThumbnailPng() сама логирует причину и зануляет буфер).
	const double CaptureStartSeconds = FPlatformTime::Seconds();
	TArray64<uint8> ThumbnailPng;
	CaptureThumbnailPng(ThumbnailPng);
	const double CaptureSeconds = FPlatformTime::Seconds() - CaptureStartSeconds;

	// В файл узор уходит ПЕРЕНЕСЁННЫМ В НАЧАЛО КООРДИНАТ: рой мог вырасти за
	// тысячи клеток от нуля, и без переноса открытый в другой сессии файл
	// оказался бы далеко за кадром (см. ComputeCenteringOffset() - там же про
	// чётность сдвига). Сдвиг применяется к КОПИИ: сам InitialStateCells не
	// трогается, сохранение по-прежнему не мутирует ничего - R после Ctrl+S
	// возвращает туда же, куда возвращал до него.
	const FIntVector CenteringOffset = AutomatonStateSerializer::ComputeCenteringOffset(InitialStateCells);

	TArray<FIntVector> CenteredCells;
	CenteredCells.Reserve(InitialStateCells.Num());
	for (const FIntVector& Cell : InitialStateCells)
	{
		CenteredCells.Add(Cell + CenteringOffset);
	}

	// Cells строится из того же перенесённого набора (возраст 0) - обе секции
	// файла обязаны быть в одной системе координат. Grid тут вообще не
	// участвует.
	TArray<AutomatonStateSerializer::FSavedCell> Cells;
	Cells.Reserve(CenteredCells.Num());
	for (const FIntVector& Cell : CenteredCells)
	{
		AutomatonStateSerializer::FSavedCell& Saved = Cells.AddDefaulted_GetRef();
		Saved.Cell = Cell;
		Saved.Age = 0;
	}
	const FAutomatonSaveHeader Header = BuildSaveHeader();

	const double WriteStartSeconds = FPlatformTime::Seconds();
	TArray64<uint8> Bytes;
	// Перенесённый паттерн пишется и как основной снимок, и как отдельная
	// InitialCells-секция (см. doc-comment namespace'а в
	// AutomatonStateSerializer.h) - это один и тот же набор клеток; формат
	// при этом не меняется (совместимость со старыми файлами, где секции
	// хранили разные вещи).
	if (!AutomatonStateSerializer::WriteSave(Header, Cells, CenteredCells, ThumbnailPng, Bytes))
	{
		return false; // причина уже в логе
	}
	if (!FFileHelper::SaveArrayToFile(Bytes, *FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("WriteStateToFile: не удалось записать файл %s"), *FilePath);
		return false;
	}
	const double WriteSeconds = FPlatformTime::Seconds() - WriteStartSeconds;

	// Sibling .png - то, что реально даёт значок-превью в Проводнике (COM
	// IThumbnailProvider для расширения .casave сознательно не делается -
	// непропорциональная инженерия для этого проекта). Те же уже
	// закодированные байты, без повторного кодирования. Отказ - тоже
	// warn-and-continue: сам .casave уже успешно записан.
	if (ThumbnailPng.Num() > 0)
	{
		const FString ThumbnailPath = FPaths::SetExtension(FilePath, TEXT("png"));
		if (!FFileHelper::SaveArrayToFile(ThumbnailPng, *ThumbnailPath))
		{
			UE_LOG(LogTemp, Warning, TEXT("WriteStateToFile: не удалось записать миниатюру %s (сохранение .casave прошло успешно)"), *ThumbnailPath);
		}
	}

	// Паспорт серии лежит в папке снимков и целью для следующего тихого Ctrl+S
	// быть не должен - см. doc-comment параметра.
	if (bUpdateLastSavePath)
	{
		LastSaveFilePath = FilePath;
	}

	UE_LOG(LogTemp, Log, TEXT("WriteStateToFile: %d клеток (перенос в центр: %s; миниатюра: %lld байт) -> %s (%.1f КБ; скриншот: %.2f мс, запись: %.2f мс)"),
		Cells.Num(), *CenteringOffset.ToString(), ThumbnailPng.Num(), *FilePath, Bytes.Num() / 1024.0, CaptureSeconds * 1000.0, WriteSeconds * 1000.0);
	return true;
}

void AAutomataOrchestrator::SaveState()
{
	if (LastSaveFilePath.IsEmpty())
	{
		// Некуда тихо перезаписывать - первый Ctrl+S в сессии ведёт себя как
		// Ctrl+Shift+S и спрашивает путь один раз.
		SaveStateAs();
		return;
	}

	WriteStateToFile(LastSaveFilePath);
}

void AAutomataOrchestrator::SaveStateAs()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogTemp, Warning, TEXT("SaveStateAs: системные диалоги выбора файла недоступны"));
		return;
	}

	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	const FString DefaultFileName = FString::Printf(TEXT("Automaton_%s.casave"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	TArray<FString> PickedFiles;
	const bool bPicked = DesktopPlatform->SaveFileDialog(
		ParentWindowHandle,
		TEXT("Сохранить состояние автомата"),
		EnsureSaveDirectory(),
		DefaultFileName,
		TEXT("Automaton Save (*.casave)|*.casave"),
		EFileDialogFlags::None,
		PickedFiles);
	if (!bPicked || PickedFiles.Num() == 0)
	{
		// Отмена диалога - намерение пользователя, не ошибка.
		UE_LOG(LogTemp, Log, TEXT("SaveStateAs: отменено пользователем"));
		return;
	}

	FString FilePath = FPaths::ConvertRelativePathToFull(PickedFiles[0]);
	if (FPaths::GetExtension(FilePath).IsEmpty())
	{
		FilePath += TEXT(".casave");
	}

	WriteStateToFile(FilePath);
}

void AAutomataOrchestrator::LoadStateFromFile()
{
	// Загрузка несовместима с идущей симуляцией - останавливаем Play и
	// автошаг Shift+F, как в BakeCellsToMesh(). Stop() сам довершает
	// чанковый разлив; защитный вызов ниже покрывает разлив, идущий вне Play
	// (после Next() Play уже нет, а Tick ещё досыпает инстансы) - иначе его
	// хвост досыпал бы старые инстансы поверх загруженного состояния.
	if (bSimulationRunning)
	{
		Stop();
	}
	if (bFastStepActive)
	{
		StopFastStep();
	}
	FinishChunkedRenderImmediately();

	// Загрузка СВАПАЕТ Grid, который фоновый шаг может читать в этот момент -
	// guard обязателен (в отличие от SaveState()/SaveStateAs(), см. их
	// doc-comment).
	if (bStepInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: фоновый шаг ещё считается - подождите и нажмите Ctrl+O ещё раз"));
		return;
	}

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: CellMesh не задан - назначьте StaticMesh в Details panel"));
		return;
	}

	if (!CellMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: CellMaterial не назначен - назначьте материал клеток в Details panel"));
		return;
	}

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: системные диалоги выбора файла недоступны"));
		return;
	}

	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	TArray<FString> PickedFiles;
	const bool bPicked = DesktopPlatform->OpenFileDialog(
		ParentWindowHandle,
		TEXT("Загрузить состояние автомата"),
		EnsureSaveDirectory(),
		TEXT(""),
		TEXT("Automaton Save (*.casave)|*.casave"),
		EFileDialogFlags::None,
		PickedFiles);
	if (!bPicked || PickedFiles.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("LoadStateFromFile: отменено пользователем"));
		return;
	}

	const FString FilePath = FPaths::ConvertRelativePathToFull(PickedFiles[0]);
	TArray64<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("LoadStateFromFile: не удалось прочитать файл %s"), *FilePath);
		return;
	}

	FAutomatonSaveHeader Header;
	TArray<AutomatonStateSerializer::FSavedCell> Cells;
	TArray<FIntVector> LoadedInitialCells;
	TArray64<uint8> LoadedThumbnailPng;
	if (!AutomatonStateSerializer::ReadSave(Bytes, Header, Cells, LoadedInitialCells, LoadedThumbnailPng))
	{
		// Причина уже в логе. До этой точки никакое состояние оркестратора не
		// тронуто - отказ по битому/чужому файлу полностью безопасен.
		return;
	}

	// Порядок обязателен: сначала параметры (CreateGrid() читает живые
	// CellSize/ChunkSize), потом сетка.
	ApplySaveHeader(Header);
	ClearBakedMesh();
	ClearGhostShape();
	Grid = CreateGrid();
	StepsSinceLastRender = 0;
	SelectedCells.Reset();
	ResetGenerationCounter();

	const double ApplyStartSeconds = FPlatformTime::Seconds();
	AutomatonStateSerializer::ApplyCells(Cells, *Grid);
	const double ApplySeconds = FPlatformTime::Seconds() - ApplyStartSeconds;

	// Точка возврата R - из ФАЙЛА (LoadedInitialCells), а не заново выведена
	// из загруженного снимка: файл хранит их раздельно именно для этого (см.
	// AutomatonStateSerializer.h) - R после загрузки должен вернуть к тому
	// же изначальному паттерну, что и до сохранения, а не к уже
	// проэволюционировавшему снимку. R реиграет с возрастами 0, как обычно;
	// точные возрасты снимка - повторный Ctrl+O.
	InitialStateCells = MoveTemp(LoadedInitialCells);

	// Последующий Ctrl+S тихо перезапишет именно этот файл.
	LastSaveFilePath = FilePath;

	RenderGridImmediate();

	UE_LOG(LogTemp, Log, TEXT("LoadStateFromFile: %d клеток из %s (заливка: %.2f мс); точка возврата для R - %d клеток; миниатюра в файле: %lld байт (пока не используется - задел под будущий UI со списком сохранений)"),
		Cells.Num(), *FilePath, ApplySeconds * 1000.0, InitialStateCells.Num(), LoadedThumbnailPng.Num());
}
