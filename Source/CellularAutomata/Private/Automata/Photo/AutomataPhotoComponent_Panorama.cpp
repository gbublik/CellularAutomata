#include "Automata/Photo/AutomataPhotoComponent.h"

#include "Orchestration/AutomataOrchestrator.h"
#include "Automata/Rendering/RenderPresets.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "Async/ParallelFor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
// GetMax2DTextureDimension() - предел стороны грани, см. TakePanoramaShot().
#include "RHIGlobals.h"

// Сферическая панорама (Shift+F10). Отдельный файл при общем заголовке - тот
// же приём, что у восемнадцати файлов оркестратора: обряд подготовки кадра у
// панорамы общий с обычным снимком (и живёт в AutomataPhotoComponent.cpp), а
// вот сшивка шести граней в равнопромежуточную проекцию не имеет с
// HighResShot'ом ничего общего и мешалась бы там.
//
// ПОЧЕМУ ШЕСТЬ ОБЫЧНЫХ ЗАХВАТОВ, А НЕ ОДИН КУБИЧЕСКИЙ. USceneCaptureComponentCube
// снял бы всё за один вызов, но раскладку граней куба (какая грань куда
// смотрит и как она при этом повёрнута) задаёт движок, и сшивать пришлось бы
// по этой раскладке, взятой на веру. Здесь базис каждой грани берётся из
// матрицы ТОГО ЖЕ поворота, которым грань снята, - направление вперёд, вправо
// и вверх приходят одной FRotationMatrix. Перепутать оси при таком устройстве
// негде: если бы поворот означал что-то другое, чем я думаю, то и снято, и
// сшито было бы одинаково неправильно, то есть правильно.
//
// Стоимость та же: кубический захват внутри тоже рисует шесть граней.

namespace
{
	/** Билинейная выборка из грани с зажимом по краю.
	 *
	 *  Зажим, а не выборка из соседней грани: на самом ребре куба билинейный
	 *  фильтр захватил бы полпикселя за границей, и вернуть их неоткуда - грань
	 *  соседа лежит в другой плоскости и другом массиве. Ошибка от зажима
	 *  ограничена половиной пикселя грани и приходится ровно на двенадцать
	 *  рёбер; при PanoramaSupersample == 2 она уходит под пиксель выходной
	 *  картинки. Тянуть сюда полноценную выборку через соседей значило бы
	 *  написать межгранную адресацию ради шва, которого при этом всё равно не
	 *  видно. */
	FORCEINLINE FColor SampleFaceBilinear(const TArray<FColor>& Face, int32 Size, float U, float V)
	{
		const float ClampedU = FMath::Clamp(U, 0.0f, float(Size) - 1.0f);
		const float ClampedV = FMath::Clamp(V, 0.0f, float(Size) - 1.0f);

		const int32 X0 = FMath::FloorToInt(ClampedU);
		const int32 Y0 = FMath::FloorToInt(ClampedV);
		const int32 X1 = FMath::Min(X0 + 1, Size - 1);
		const int32 Y1 = FMath::Min(Y0 + 1, Size - 1);

		const float FracX = ClampedU - float(X0);
		const float FracY = ClampedV - float(Y0);

		const FColor& C00 = Face[Y0 * Size + X0];
		const FColor& C10 = Face[Y0 * Size + X1];
		const FColor& C01 = Face[Y1 * Size + X0];
		const FColor& C11 = Face[Y1 * Size + X1];

		auto Lerp2 = [FracX, FracY](uint8 A00, uint8 A10, uint8 A01, uint8 A11) -> uint8
		{
			const float Top = float(A00) + (float(A10) - float(A00)) * FracX;
			const float Bottom = float(A01) + (float(A11) - float(A01)) * FracX;
			return uint8(FMath::Clamp(FMath::RoundToInt(Top + (Bottom - Top) * FracY), 0, 255));
		};

		// Альфа не смешивается, а ставится непрозрачной: у SCS_FinalColorLDR она
		// не значит "прозрачность кадра", и PNG с полупрозрачным небом был бы
		// сюрпризом, а не результатом.
		return FColor(
			Lerp2(C00.R, C10.R, C01.R, C11.R),
			Lerp2(C00.G, C10.G, C01.G, C11.G),
			Lerp2(C00.B, C10.B, C01.B, C11.B),
			255);
	}

	/** Свободное имя вида Panorama_0001.png в папке снимков. Номер, а не
	 *  отметка времени: панорамы снимают подряд, сравнивая ракурсы, и
	 *  порядковый номер сортируется и произносится вслух, а timestamp - нет. */
	FString MakePanoramaFilePath()
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir());
		IFileManager::Get().MakeDirectory(*Dir, true);

		for (int32 Index = 1; Index < 10000; ++Index)
		{
			const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("Panorama_%04d.png"), Index));
			if (!IFileManager::Get().FileExists(*Path))
			{
				return Path;
			}
		}

		// Десять тысяч панорам в одной папке - случай не рабочий, но молча
		// затирать чужой файл всё равно нельзя.
		return FPaths::Combine(Dir, TEXT("Panorama_overflow.png"));
	}
}

void UAutomataPhotoComponent::BuildPanoramaFaces(float Yaw, TArray<FPanoramaFace>& OutFaces)
{
	OutFaces.Reset(6);

	// Четыре по горизонту от рыскания камеры плюс зенит и надир. Тангаж и крен
	// камеры сюда не попадают намеренно - см. doc-comment TakePanoramaShot():
	// наклонённая панорама это заваленный мир, а не наклонённый кадр.
	OutFaces.Add({ FRotator(0.0f, Yaw, 0.0f), TEXT("вперёд") });
	OutFaces.Add({ FRotator(0.0f, Yaw + 90.0f, 0.0f), TEXT("вправо") });
	OutFaces.Add({ FRotator(0.0f, Yaw + 180.0f, 0.0f), TEXT("назад") });
	OutFaces.Add({ FRotator(0.0f, Yaw + 270.0f, 0.0f), TEXT("влево") });
	OutFaces.Add({ FRotator(90.0f, Yaw, 0.0f), TEXT("вверх") });
	OutFaces.Add({ FRotator(-90.0f, Yaw, 0.0f), TEXT("вниз") });
}

bool UAutomataPhotoComponent::EnsurePanoramaCapture(AAutomataOrchestrator& Owner, int32 FaceSize)
{
	// Цель пересоздаётся только при смене стороны: между снимками одного
	// размера она переживает и сам снимок, и правку прочих настроек.
	if (!IsValid(PanoramaTarget) || PanoramaTarget->SizeX != FaceSize)
	{
		PanoramaTarget = NewObject<UTextureRenderTarget2D>(&Owner, TEXT("AutomataPanoramaTarget"));
		if (!PanoramaTarget)
		{
			return false;
		}

		// RTF_RGBA8, а НЕ RTF_RGBA8_SRGB: SCS_FinalColorLDR отдаёт уже
		// гамма-кодированный цвет, а запись в sRGB-цель применила бы кодирование
		// второй раз - панорама вышла бы выбеленной. Тот же выбор, что делает
		// движковый Create Render Target 2D под захват сцены.
		PanoramaTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
		PanoramaTarget->ClearColor = FLinearColor::Black;
		PanoramaTarget->bAutoGenerateMips = false;
		PanoramaTarget->InitAutoFormat(FaceSize, FaceSize);
		PanoramaTarget->UpdateResourceImmediate(true);
	}

	if (!IsValid(PanoramaCapture))
	{
		// Лениво, как и сам компонент съёмки: default-subobject в конструкторе
		// Live Coding не умеет доставить на уже размещённого в уровне актора.
		PanoramaCapture = NewObject<USceneCaptureComponent2D>(&Owner, TEXT("AutomataPanoramaCapture"));
		if (!PanoramaCapture)
		{
			return false;
		}

		PanoramaCapture->SetupAttachment(Owner.GetRootComponent());
		PanoramaCapture->RegisterComponent();

		// По требованию, а не каждый кадр: снимок делается шестью явными
		// CaptureScene(), между снимками захвату работать не над чем.
		PanoramaCapture->bCaptureEveryFrame = false;
		PanoramaCapture->bCaptureOnMovement = false;
		PanoramaCapture->bAlwaysPersistRenderingState = true;

		// 90 градусов при квадратной цели - и есть грань куба: горизонтальный и
		// вертикальный углы совпадают, шесть таких граней смыкаются без зазора.
		PanoramaCapture->ProjectionType = ECameraProjectionMode::Perspective;
		PanoramaCapture->FOVAngle = 90.0f;
		PanoramaCapture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

		// Экранные эффекты - вон. Каждый из них считается в плоскости кадра: у
		// панорамы кадров шесть, и на стыке грани такой эффект обрывается
		// ступенькой. Особенно заметен bloom (яркая клетка у края грани светит
		// только внутрь неё) и SSAO (затенение в углу обрывается по ребру).
		PanoramaCapture->ShowFlags.SetBloom(false);
		PanoramaCapture->ShowFlags.SetScreenSpaceReflections(false);
		PanoramaCapture->ShowFlags.SetAmbientOcclusion(false);
		PanoramaCapture->ShowFlags.SetMotionBlur(false);
		PanoramaCapture->ShowFlags.SetDepthOfField(false);
		PanoramaCapture->ShowFlags.SetVignette(false);
		PanoramaCapture->ShowFlags.SetEyeAdaptation(false);
	}

	PanoramaCapture->TextureTarget = PanoramaTarget;
	return true;
}

bool UAutomataPhotoComponent::CapturePanoramaFace(const FPanoramaFace& Face, const FVector& Origin,
	int32 FaceSize, float ExposureBias, TArray<FColor>& OutPixels,
	FVector& OutForward, FVector& OutRight, FVector& OutUp)
{
	if (!IsValid(PanoramaCapture) || !IsValid(PanoramaTarget))
	{
		return false;
	}

	PanoramaCapture->SetWorldLocationAndRotation(Origin, Face.Rotation);

	// Экспозиция ручная и одна на все шесть граней. Автоэкспозиция замеряет
	// яркость КАДРА - грань в тёмную сторону вытянулась бы, грань в светлую
	// притухла, и панорама пошла бы ступеньками по кругу. Физическую камеру при
	// этом отключаем: иначе к ручному значению домножились бы диафрагма с
	// выдержкой, и знать, что означает поправка, стало бы нельзя.
	FPostProcessSettings& PostProcess = PanoramaCapture->PostProcessSettings;
	PostProcess.bOverride_AutoExposureMethod = true;
	PostProcess.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	PostProcess.bOverride_AutoExposureBias = true;
	PostProcess.AutoExposureBias = ExposureBias;
	PostProcess.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	PostProcess.AutoExposureApplyPhysicalCameraExposure = 0;

	PanoramaCapture->CaptureScene();

	FTextureRenderTargetResource* Resource = PanoramaTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return false;
	}

	// Чтение синхронное: оно само дожидается видеокарты, поэтому отдельного
	// ожидания кадра (как у HighResShot с его тиком) здесь не нужно вовсе.
	OutPixels.Reset();
	if (!Resource->ReadPixels(OutPixels) || OutPixels.Num() != FaceSize * FaceSize)
	{
		return false;
	}

	// Базис - из той же матрицы, что ушла в захват. См. шапку файла: это и есть
	// причина, по которой сшивка не может разойтись со съёмкой.
	const FRotationMatrix Basis(Face.Rotation);
	OutForward = Basis.GetUnitAxis(EAxis::X);
	OutRight = Basis.GetUnitAxis(EAxis::Y);
	OutUp = Basis.GetUnitAxis(EAxis::Z);

	return true;
}

void UAutomataPhotoComponent::TakePanoramaShot()
{
	AAutomataOrchestrator* Owner = ResolveOrchestrator();
	if (!Owner)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePanoramaShot: компонент съёмки не на оркестраторе - панорама отменена"));
		return;
	}

	// 0. Проверки ДО побочных эффектов - тот же принцип, что у обычного снимка:
	// отказ обязан оставить прогон, отсечения и профиль нетронутыми.
	const int32 Width = FMath::Clamp(Owner->PanoramaWidth, 512, 16384) & ~1;
	const int32 Height = Width / 2;
	const int32 Supersample = FMath::Clamp(Owner->PanoramaSupersample, 1, 2);

	// Сторона грани: Width/4 даёт в центре грани ровно ту же плотность
	// пикселей, что у панорамы на экваторе (Width/4 против Width/2pi на радиан).
	// Совпадение не приблизительное - оно следует из того, что грань покрывает
	// 90 градусов, а панорама 360.
	const int32 FaceSize = (Width / 4) * Supersample;

	const int32 MaxSide = static_cast<int32>(GetMax2DTextureDimension());
	if (FaceSize > MaxSide)
	{
		const FString Reason = FString::Printf(
			TEXT("грань %d не влезает: предел стороны текстуры на этой видеокарте %d"), FaceSize, MaxSide);
		UE_LOG(LogTemp, Warning, TEXT("TakePanoramaShot: %s - панорама отменена"), *Reason);
		Owner->ShowStatusMessage(AAutomataOrchestrator::StatusKey_Panorama,
			FString::Printf(TEXT("[Shift+F10] %s"), *Reason));
		return;
	}

	const int32 PhotoPresetIndex = RenderPresets::GetPhotoPresetIndex(Owner->bPhotoLeanMemory);
	if (PhotoPresetIndex == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePanoramaShot: профиль съёмки не найден в таблице - панорама отменена"));
		return;
	}

	// Точку съёмки берём у камеры. Только положение и рыскание: тангаж и крен
	// отбрасываются, иначе в просмотрщике завалится весь мир - см. doc-comment
	// AAutomataOrchestrator::TakePanoramaShot().
	FVector Origin = FVector::ZeroVector;
	FVector Forward = FVector::ForwardVector;
	if (!Owner->GetCameraView(Origin, Forward))
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePanoramaShot: камера недоступна (вне PIE?) - панорама отменена"));
		Owner->ShowStatusMessage(AAutomataOrchestrator::StatusKey_Panorama,
			TEXT("[Shift+F10] Панорама не снята: камера недоступна"));
		return;
	}
	const float Yaw = Forward.Rotation().Yaw;

	const double StartSeconds = FPlatformTime::Seconds();

	// 1. Кадр обязан быть неподвижен: шесть граней снимаются последовательно, и
	// шагающая симуляция склеила бы панораму из разных поколений.
	if (Owner->IsSimulationRunning())
	{
		Owner->Stop();
	}
	if (Owner->IsFastStepActive())
	{
		Owner->StopFastStep();
	}

	// 2. Профиль съёмки - как у F10, с той же оговоркой про фон: профиль обязан
	// задавать все свои поля, поэтому выбор по фону запоминается и возвращается
	// сеттером (он же поднимет звёздочку "профиль изменён" в HUD).
	const bool bBackgroundWasVisible = Owner->IsBackgroundVisible();
	Owner->ApplyRenderPreset(PhotoPresetIndex);
	if (Owner->IsBackgroundVisible() != bBackgroundWasVisible)
	{
		Owner->SetBackgroundVisible(bBackgroundWasVisible);
	}

	// 3. Инструменты редактирования из кадра вон - и это здесь важнее, чем у
	// F10: коробка куба окружает камеру со всех сторон, в панораму она попала бы
	// не краем, а целиком.
	HideEditingVisuals(*Owner);

	Owner->RenderGridImmediate();

	if (!EnsurePanoramaCapture(*Owner, FaceSize))
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePanoramaShot: не удалось создать захват сцены - панорама отменена"));
		RestoreEditingVisuals(*Owner);
		return;
	}

	const int32 CellCount = Owner->Grid.IsValid() ? Owner->Grid->Num() : 0;
	UE_LOG(LogTemp, Log, TEXT("=== Панорама: %dx%d, грань %d (x%d) | профиль %s | живых клеток %d ==="),
		Width, Height, FaceSize, Supersample,
		Owner->bPhotoLeanMemory ? TEXT("Photo Lean") : TEXT("Photo"), CellCount);
	Owner->ShowStatusMessage(AAutomataOrchestrator::StatusKey_Panorama,
		FString::Printf(TEXT("[Shift+F10] Панорама %dx%d - снимаю шесть граней"), Width, Height));

	// 4. Шесть граней.
	TArray<FPanoramaFace> Faces;
	BuildPanoramaFaces(Yaw, Faces);

	TArray<TArray<FColor>> FacePixels;
	TArray<FVector> FaceForward;
	TArray<FVector> FaceRight;
	TArray<FVector> FaceUp;
	FacePixels.SetNum(Faces.Num());
	FaceForward.SetNum(Faces.Num());
	FaceRight.SetNum(Faces.Num());
	FaceUp.SetNum(Faces.Num());

	for (int32 Index = 0; Index < Faces.Num(); ++Index)
	{
		if (!CapturePanoramaFace(Faces[Index], Origin, FaceSize, Owner->PanoramaExposureBias,
			FacePixels[Index], FaceForward[Index], FaceRight[Index], FaceUp[Index]))
		{
			UE_LOG(LogTemp, Warning, TEXT("TakePanoramaShot: грань '%s' не снялась - панорама отменена"), Faces[Index].Name);
			Owner->ShowStatusMessage(AAutomataOrchestrator::StatusKey_Panorama,
				TEXT("[Shift+F10] Панорама НЕ снята - смотрите лог"));
			RestoreEditingVisuals(*Owner);
			return;
		}
	}

	const double CaptureSeconds = FPlatformTime::Seconds() - StartSeconds;

	// 5. Сшивка. Экран больше не нужен - инструменты можно вернуть сразу, не
	// заставляя человека смотреть на голую сцену всё время кодирования PNG.
	RestoreEditingVisuals(*Owner);

	// Базис самой панорамы: горизонт по рысканию камеры, верх - мировой. Строится
	// той же FRotationMatrix, что и грани, поэтому центр картинки гарантированно
	// совпадает с гранью "вперёд", а не оказывается сдвинут на полкадра.
	const FRotationMatrix PanoramaBasis(FRotator(0.0f, Yaw, 0.0f));
	const FVector PanoForward = PanoramaBasis.GetUnitAxis(EAxis::X);
	const FVector PanoRight = PanoramaBasis.GetUnitAxis(EAxis::Y);
	const FVector PanoUp = PanoramaBasis.GetUnitAxis(EAxis::Z);

	TArray<FColor> Panorama;
	Panorama.SetNumUninitialized(Width * Height);

	const double StitchStartSeconds = FPlatformTime::Seconds();

	// По строкам: 34 мегапикселя, каждый со своей выборкой грани, - на одном
	// потоке это секунды, и они здесь ни за что.
	ParallelFor(Height, [&](int32 Y)
	{
		// Широта: +90 градусов на верхней строке, -90 на нижней. Полпикселя - это
		// центр пикселя, а не его угол: без них картинка уезжает на полстроки.
		const double Latitude = (0.5 - (double(Y) + 0.5) / double(Height)) * PI;
		const double CosLat = FMath::Cos(Latitude);
		const double SinLat = FMath::Sin(Latitude);

		FColor* Row = Panorama.GetData() + Y * Width;

		for (int32 X = 0; X < Width; ++X)
		{
			// Долгота: 0 в центре картинки, растёт вправо - то есть по рысканию,
			// как её и будет крутить просмотрщик.
			const double Longitude = ((double(X) + 0.5) / double(Width) - 0.5) * 2.0 * PI;

			const FVector Direction =
				(PanoForward * (CosLat * FMath::Cos(Longitude))) +
				(PanoRight * (CosLat * FMath::Sin(Longitude))) +
				(PanoUp * SinLat);

			// Грань выбирается по наибольшей проекции на её направление взгляда -
			// то же самое, что "по наибольшей координате" у кубической выборки,
			// только без единого допущения о том, как именно уложены грани.
			int32 BestFace = 0;
			double BestDot = TNumericLimits<double>::Lowest();
			for (int32 Index = 0; Index < 6; ++Index)
			{
				const double Dot = FVector::DotProduct(Direction, FaceForward[Index]);
				if (Dot > BestDot)
				{
					BestDot = Dot;
					BestFace = Index;
				}
			}

			// Луч ровно по касательной ко всем граням - случай геометрически
			// невозможный (шесть граней покрывают сферу целиком), но деление на
			// ноль стоит дешевле, чем разбор того, откуда взялся чёрный пиксель.
			if (BestDot <= KINDA_SMALL_NUMBER)
			{
				Row[X] = FColor::Black;
				continue;
			}

			const double PlaneX = FVector::DotProduct(Direction, FaceRight[BestFace]) / BestDot;
			const double PlaneY = FVector::DotProduct(Direction, FaceUp[BestFace]) / BestDot;

			// Из [-1, 1] в пиксели грани. Ось V перевёрнута: строка 0 у
			// прочитанного кадра сверху, а +Up смотрит вверх.
			const float U = float((PlaneX + 1.0) * 0.5 * double(FaceSize) - 0.5);
			const float V = float((1.0 - PlaneY) * 0.5 * double(FaceSize) - 0.5);

			Row[X] = SampleFaceBilinear(FacePixels[BestFace], FaceSize, U, V);
		}
	});

	const double StitchSeconds = FPlatformTime::Seconds() - StitchStartSeconds;

	// 6. Файл. Тем же путём, что и срезы F6/F7 - PNGCompressImageArray плюс
	// SaveArrayToFile, - но в папку снимков: панорама это фотография, и лежать
	// ей рядом с фотографиями.
	TArray64<uint8> PngBytes;
	FImageUtils::PNGCompressImageArray(Width, Height,
		TArrayView64<const FColor>(Panorama.GetData(), Panorama.Num()), PngBytes);

	if (PngBytes.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePanoramaShot: не удалось закодировать PNG"));
		Owner->ShowStatusMessage(AAutomataOrchestrator::StatusKey_Panorama,
			TEXT("[Shift+F10] Панорама НЕ сохранена: ошибка кодирования PNG"));
		return;
	}

	const FString FilePath = MakePanoramaFilePath();
	if (!FFileHelper::SaveArrayToFile(PngBytes, *FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("TakePanoramaShot: не удалось записать файл '%s'"), *FilePath);
		Owner->ShowStatusMessage(AAutomataOrchestrator::StatusKey_Panorama,
			TEXT("[Shift+F10] Панорама НЕ сохранена: файл не записан"));
		return;
	}

	const double TotalSeconds = FPlatformTime::Seconds() - StartSeconds;
	const double FileSizeMB = double(PngBytes.Num()) / (1024.0 * 1024.0);

	UE_LOG(LogTemp, Log, TEXT("=== Панорама готова за %.1f с (грани %.1f с, сшивка %.1f с): %s (%.1f МБ) ==="),
		TotalSeconds, CaptureSeconds, StitchSeconds, *FilePath, FileSizeMB);
	Owner->ShowStatusMessage(AAutomataOrchestrator::StatusKey_Panorama,
		FString::Printf(TEXT("[Shift+F10] Панорама готова за %.1f с (%.1f МБ): %s"),
			TotalSeconds, FileSizeMB, *FPaths::GetCleanFilename(FilePath)));
}
