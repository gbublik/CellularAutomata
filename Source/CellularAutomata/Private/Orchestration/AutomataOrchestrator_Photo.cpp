// Fill out your copyright notice in the Description page of Project Settings.


#include "CellularAutomata/Public/Orchestration/AutomataOrchestrator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Automata/Rendering/RenderCullVolume.h"
#include "Automata/Rendering/RenderPresets.h"
// GetMax2DTextureDimension() - предел стороны снимка, см. TakePhotoShot().
#include "RHIGlobals.h"
// RHIGetTextureMemoryStats() - расход видеопамяти в лог при съёмке.
#include "DynamicRHI.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"


void AAutomataOrchestrator::TakePhotoShot()
{
	// 0. Проверяем размер ДО любых побочных эффектов - тот же принцип, что у
	// бюджетов бейка и генератора: отказ обязан оставить всё как было, а не
	// остановить симуляцию, снять отсечения и переключить профиль ради снимка,
	// которого не будет.
	int32 Width = 0;
	int32 Height = 0;
	if (!ValidatePhotoShotResolution(Width, Height))
	{
		return;
	}

	// 1. Кадр обязан быть неподвижен. HighResShot снимает тайлами, по одному
	// проходу на тайл, и между проходами проходят кадры игры - шагающая
	// симуляция склеилась бы из разных поколений.
	if (bSimulationRunning)
	{
		Stop();
	}
	if (bFastStepActive)
	{
		StopFastStep();
	}

	// 2. Всё, что настроено для РАЗГЛЯДЫВАНИЯ, остаётся как есть: куб отсечения
	// (C), срез вдоль взгляда (J), фильтр по возрасту (цифры) и спрятанный фон
	// (U). Снимают ровно то, что видят, - если человек вырезал кубом кусок
	// структуры и убрал небо, значит именно это и хочет получить картинкой.
	//
	// Первая версия гасила срез и фильтр по возрасту "чтобы в кадр попало всё",
	// и это было ошибкой: она молча выбрасывала настройку, ради которой кадр и
	// выстраивали. Куб и так не трогался - профили рендера им не владеют.
	//
	// Профиль ниже владеет только КАЧЕСТВОМ (свет, тени, Lumen, сглаживание) и
	// двумя ускорителями, которые портят снимок, - отсечением по расстоянию и
	// Ghost Shape. Фон в профиле тоже есть, поэтому его выбор приходится
	// восстанавливать вручную: профиль обязан задавать все свои поля целиком
	// (см. doc-comment FRenderPreset), исключений в таблице не предусмотрено.
	const bool bBackgroundWasVisible = bShowBackground;

	// 3. Профиль съёмки - обычный или экономный по памяти. Своей F-клавиши ни у
	// одного из них нет, оба применяются только отсюда.
	const int32 PhotoPresetIndex = RenderPresets::GetPhotoPresetIndex(bPhotoLeanMemory);
	if (PhotoPresetIndex == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePhotoShot: профиль съёмки не найден в таблице - снимок отменён"));
		return;
	}
	ApplyRenderPreset(PhotoPresetIndex);

	// 3.5. Вернуть выбор по фону. Через сеттер, а не записью поля: он и применит
	// видимость к компонентам, и поднимет bRenderPresetModified - профиль после
	// этого действительно описывает экран не полностью, и HUD обязан показать
	// это звёздочкой, а не врать именем профиля.
	if (bShowBackground != bBackgroundWasVisible)
	{
		SetBackgroundVisible(bBackgroundWasVisible);
	}

	// 3.7. Убрать из кадра инструменты редактирования - коробку куба, её ручки и
	// подсветку выделения. До рендера ниже, чтобы перерисовка сразу учла это.
	HideEditingVisualsForPhoto();

	// 4. Перерисовать целиком и немедленно: ApplyRenderPreset() уже
	// перерисовывает через сеттер Ghost Shape, но полагаться на это нельзя -
	// он перерисует только если флаг силуэта реально сменился.
	const double RenderStartSeconds = FPlatformTime::Seconds();
	RenderGridImmediate();
	const double RenderSeconds = FPlatformTime::Seconds() - RenderStartSeconds;

	// 5. Сколько раз отрисовать кадр перед сохранением, и сам снимок. Это не
	// пауза, а множитель времени съёмки - см. doc-comment PhotoShotDelayFrames.
	// Ставим каждый раз, а не один раз при старте: свойство редактируется в
	// Details, и значение должно означать себя на КАЖДОМ снимке, а не на первом.
	const int32 DelayFrames = FMath::Max(1, PhotoShotDelayFrames);
	RunRenderConsoleCommand(FString::Printf(TEXT("r.HighResScreenshotDelay %d"), DelayFrames));

	const int32 CellCount = Grid.IsValid() ? Grid->Num() : 0;
	const double Megapixels = (double(Width) * double(Height)) / 1000000.0;

	// Память под текстуры на момент съёмки - чтобы "жрёт немерено" стало
	// числом. Мерить надо ДО выстрела: сам снимок аллоцирует свои буферы уже
	// внутри рендер-потока, и сравнивать имеет смысл базу "экономный профиль
	// против обычного", а не пик.
	FTextureMemoryStats MemoryStats;
	RHIGetTextureMemoryStats(MemoryStats);
	const double AllocatedMB = double(MemoryStats.StreamingMemorySize + MemoryStats.NonStreamingMemorySize) / (1024.0 * 1024.0);
	const double TotalMB = MemoryStats.TotalGraphicsMemory > 0 ? double(MemoryStats.TotalGraphicsMemory) / (1024.0 * 1024.0) : 0.0;

	UE_LOG(LogTemp, Log, TEXT("=== Снимок: %dx%d (%.1f Мпикс) x%d прогонов | профиль %s | живых клеток %d ==="),
		Width, Height, Megapixels, DelayFrames, bPhotoLeanMemory ? TEXT("Photo Lean") : TEXT("Photo"), CellCount);
	UE_LOG(LogTemp, Log, TEXT("Снимок: подготовка кадра заняла %.2f с; текстур занято %.0f МБ из %.0f МБ"),
		RenderSeconds, AllocatedMB, TotalMB);
	UE_LOG(LogTemp, Log, TEXT("Снимок: команда выдана, дальше движок нарисует кадр %d раз подряд в разрешении снимка. Окно замрёт до конца, промежуточных сообщений не будет - следующая строка появится уже по готовности файла."),
		DelayFrames);

	ShowStatusMessage(StatusKey_PhotoShot, FString::Printf(TEXT("[F10] Снимок %dx%d (%.1f Мпикс) x%d прогонов - окно замрёт до конца"),
		Width, Height, Megapixels, DelayFrames));

	// Метка для замера длительности - см. PhotoShotIssuedSeconds. Ставится
	// ПЕРЕД выдачей команды, хотя команда и возвращается мгновенно: сам кадр
	// рисуется позже, и отсчёт должен начинаться отсюда.
	PhotoShotIssuedSeconds = FPlatformTime::Seconds();
	PhotoShotIssuedFrame = GFrameCounter;
	// Тик мог быть выключён - Stop() выше его гасит, а без тика некому будет
	// заметить, что съёмка кончилась, и напечатать итог.
	SetActorTickEnabled(true);

	RunRenderConsoleCommand(FString::Printf(TEXT("HighResShot %dx%d"), Width, Height));
}

void AAutomataOrchestrator::HideEditingVisualsForPhoto()
{
	bPhotoRestoreVolumeVisible = false;
	bPhotoRestoreGizmoVisible = false;
	bPhotoRestoreSelectionVisible = false;

	// EnsureRenderCullVolume(), а не GetActiveCullVolume(): коробку надо
	// спрятать независимо от того, режет она сейчас или нет - в кадре она
	// мешает в любом случае.
	if (ARenderCullVolume* CullVolume = EnsureRenderCullVolume())
	{
		bPhotoRestoreVolumeVisible = CullVolume->IsVolumeVisible();
		bPhotoRestoreGizmoVisible = CullVolume->IsGizmoVisible();

		if (bPhotoRestoreGizmoVisible)
		{
			CullVolume->SetGizmoVisible(false);
		}
		if (bPhotoRestoreVolumeVisible)
		{
			CullVolume->SetVolumeVisible(false);
		}
	}

	// Подсветку выделения гасим самим компонентом, не трогая SelectedCells:
	// выделение - это состояние работы, снимок не повод его терять.
	if (SelectionMeshComponent)
	{
		bPhotoRestoreSelectionVisible = SelectionMeshComponent->IsVisible();
		if (bPhotoRestoreSelectionVisible)
		{
			SelectionMeshComponent->SetVisibility(false);
		}
	}
}

void AAutomataOrchestrator::RestoreEditingVisualsAfterPhoto()
{
	if (ARenderCullVolume* CullVolume = EnsureRenderCullVolume())
	{
		if (bPhotoRestoreVolumeVisible)
		{
			CullVolume->SetVolumeVisible(true);
		}
		if (bPhotoRestoreGizmoVisible)
		{
			CullVolume->SetGizmoVisible(true);
		}
	}

	if (SelectionMeshComponent && bPhotoRestoreSelectionVisible)
	{
		SelectionMeshComponent->SetVisibility(true);
	}

	bPhotoRestoreVolumeVisible = false;
	bPhotoRestoreGizmoVisible = false;
	bPhotoRestoreSelectionVisible = false;
}

void AAutomataOrchestrator::ReportPhotoShotCompleted()
{
	const double ElapsedSeconds = FPlatformTime::Seconds() - PhotoShotIssuedSeconds;
	PhotoShotIssuedSeconds = 0.0;
	PhotoShotIssuedFrame = 0;

	// Ищем сам файл, а не верим, что он появился: съёмка молча не сохраняет,
	// например когда размер не влез в память, и "готово" без файла было бы
	// худшим из возможных отчётов.
	const FString ScreenshotDir = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir());
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *ScreenshotDir, TEXT("*.png"), true, false);

	FString NewestFile;
	FDateTime NewestTime = FDateTime::MinValue();
	for (const FString& File : Files)
	{
		const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*File);
		if (Timestamp > NewestTime)
		{
			NewestTime = Timestamp;
			NewestFile = File;
		}
	}

	// Файл считаем "нашим", только если он свежее момента выдачи команды с
	// небольшим запасом - иначе показали бы вчерашний снимок как результат.
	const bool bFresh = !NewestFile.IsEmpty()
		&& (FDateTime::UtcNow() - NewestTime).GetTotalSeconds() < ElapsedSeconds + 30.0;

	if (bFresh)
	{
		const double FileSizeMB = double(IFileManager::Get().FileSize(*NewestFile)) / (1024.0 * 1024.0);
		UE_LOG(LogTemp, Log, TEXT("=== Снимок готов за %.1f с: %s (%.1f МБ) ==="), ElapsedSeconds, *NewestFile, FileSizeMB);
		ShowStatusMessage(StatusKey_PhotoShot, FString::Printf(TEXT("[F10] Снимок готов за %.1f с (%.1f МБ)"), ElapsedSeconds, FileSizeMB));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("=== Снимок: %.1f с прошло, но нового файла в %s не появилось. Обычная причина - размер кадра не влез в память; попробуйте меньше PhotoShotResolution или включите bPhotoLeanMemory. ==="),
			ElapsedSeconds, *ScreenshotDir);
		ShowStatusMessage(StatusKey_PhotoShot, TEXT("[F10] Снимок НЕ сохранён - смотрите лог"));
	}

	// Безусловно, вне зависимости от того, сохранился файл или нет: экран обязан
	// вернуться в то состояние, в котором его оставил пользователь.
	RestoreEditingVisualsAfterPhoto();

	// Возвращаем тик в то состояние, которое ему полагается по остальным
	// причинам (см. SetViewSliceEnabled() - там та же строка).
	SetActorTickEnabled(bEnableViewSlice || bSimulationRunning || bFastStepActive);
}

bool AAutomataOrchestrator::ValidatePhotoShotResolution(int32& OutWidth, int32& OutHeight) const
{
	OutWidth = FMath::Max(64, PhotoShotResolution.X);
	OutHeight = FMath::Max(64, PhotoShotResolution.Y);

	// HighResShot НЕ тайлит - он заводит один рендер-таргет запрошенного
	// размера (UnrealClient.cpp: DummyViewport->SizeX = GScreenshotResolutionX).
	// Значит потолок ровно один - предел RHI на сторону 2D-текстуры, и упереться
	// в него означает не "снимок поменьше", а вылет по нехватке видеопамяти,
	// уже после того как профиль применён и сетка перерисована.
	const int32 MaxSide = static_cast<int32>(GetMax2DTextureDimension());
	if (OutWidth > MaxSide || OutHeight > MaxSide)
	{
		const FString Reason = FString::Printf(
			TEXT("снимок %dx%d не влезает: предел стороны текстуры на этой видеокарте %d"),
			OutWidth, OutHeight, MaxSide);
		UE_LOG(LogTemp, Warning, TEXT("TakePhotoShot: %s - снимок отменён"), *Reason);
		ShowStatusMessage(StatusKey_PhotoShot, FString::Printf(TEXT("[F10] %s"), *Reason));
		return false;
	}

	// Мягкий порог: в предел текстуры укладывается и то, что не укладывается в
	// видеопамять. Считаем по площади, а не по стороне - именно она и растёт
	// квадратично, и именно на ней погорела первая версия (ScreenPercentage 200
	// поверх 8K). Не запрещаем: сколько потянет конкретная карта, отсюда не
	// видно - но предупредить обязаны ДО того, как редактор начнёт умирать.
	constexpr double SoftLimitMegapixels = 40.0;
	const double Megapixels = (double(OutWidth) * double(OutHeight)) / 1000000.0;
	if (Megapixels > SoftLimitMegapixels)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePhotoShot: %.1f Мпикс (%dx%d) - это уже гигабайты видеопамяти под GBuffer. Если редактор вылетит, снижайте PhotoShotResolution."),
			Megapixels, OutWidth, OutHeight);
	}

	return true;
}
