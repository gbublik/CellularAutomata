#include "Automata/Rendering/RenderPresets.h"

namespace RenderPresets
{
	namespace
	{
		/** Значения движковых cvar'ов одного профиля. Именованные поля, а не
		 *  голый список строк на каждый пресет, ровно по одной причине: набор
		 *  ИМЁН обязан совпадать у всех профилей (иначе переключение оставляло
		 *  бы хвосты от предыдущего - см. doc-comment FRenderPreset), а
		 *  структура с дефолтами это гарантирует конструктивно, без сверки
		 *  четырёх списков глазами.
		 *
		 *  Значения по умолчанию здесь = профиль Quality, т.е. "всё включено":
		 *  остальные профили пишутся как отличия от него. */
		struct FRenderCvars
		{
			/** Lumen: динамическое GI и отражения (r.DynamicGlobalIlluminationMethod
			 *  в проекте уже == 1). Самая дорогая пара на GPU. */
			int32 LumenGlobalIllumination = 1;
			int32 LumenReflections = 1;

			/** Virtual Shadow Maps + общее качество теней. */
			int32 VirtualShadowMaps = 1;
			int32 ShadowQuality = 5;

			/** 0 - без сглаживания, 1 - FXAA, 2 - TAA, 4 - TSR. */
			int32 AntiAliasing = 4;

			/** Процент разрешения внутреннего рендера. Единственная настройка,
			 *  которая масштабирует ВСЁ сразу, поэтому её крутит только самый
			 *  быстрый профиль. */
			int32 ScreenPercentage = 100;

			/** Пост-обработка. Дешевле теней и Lumen, но на несколько мс тянет. */
			int32 Bloom = 5;
			int32 DepthOfField = 2;
			int32 MotionBlur = 4;
			/** -1 - как решит движок (значение по умолчанию), 0 - выключено. */
			int32 AmbientOcclusion = -1;
			int32 VolumetricFog = 1;

			/** Множитель ВСЕХ дистанций отсечения, включая
			 *  InstanceEndCullDistance клеток. В этом окружении Scalability
			 *  выставляет его в 10, из-за чего заданные вручную дистанции
			 *  отсечения срабатывали в десять раз дальше, чем написано (см.
			 *  CLAUDE.md, разбор "не отсекает, сколько ни лети"). Профили,
			 *  которые реально полагаются на отсечение, ставят 1, чтобы число
			 *  в CellCullEndDistance означало ровно себя. */
			int32 ViewDistanceScale = 10;
		};

		/** Разворачивает набор в список консольных команд. Порядок здесь -
		 *  порядок применения; он не важен (cvar'ы независимы), важен лишь
		 *  полный охват. */
		TArray<FString> BuildCommands(const FRenderCvars& Cvars)
		{
			return {
				FString::Printf(TEXT("r.Lumen.DiffuseIndirect.Allow %d"), Cvars.LumenGlobalIllumination),
				FString::Printf(TEXT("r.Lumen.Reflections.Allow %d"), Cvars.LumenReflections),
				FString::Printf(TEXT("r.Shadow.Virtual.Enable %d"), Cvars.VirtualShadowMaps),
				FString::Printf(TEXT("r.ShadowQuality %d"), Cvars.ShadowQuality),
				FString::Printf(TEXT("r.AntiAliasingMethod %d"), Cvars.AntiAliasing),
				FString::Printf(TEXT("r.ScreenPercentage %d"), Cvars.ScreenPercentage),
				FString::Printf(TEXT("r.BloomQuality %d"), Cvars.Bloom),
				FString::Printf(TEXT("r.DepthOfFieldQuality %d"), Cvars.DepthOfField),
				FString::Printf(TEXT("r.MotionBlurQuality %d"), Cvars.MotionBlur),
				FString::Printf(TEXT("r.AmbientOcclusionLevels %d"), Cvars.AmbientOcclusion),
				FString::Printf(TEXT("r.VolumetricFog %d"), Cvars.VolumetricFog),
				FString::Printf(TEXT("r.ViewDistanceScale %d"), Cvars.ViewDistanceScale)
			};
		}
	}

	const TArray<FRenderPreset>& GetAll()
	{
		static const TArray<FRenderPreset> Presets = []()
		{
			TArray<FRenderPreset> Result;

			// --- F1: Quality -------------------------------------------------
			// Эталон "как задумано": ничего не отключено. Нужен не только сам
			// по себе, но и как точка возврата - без него из быстрых профилей
			// нельзя было бы вернуться одним нажатием.
			{
				FRenderPreset Preset;
				Preset.Name = TEXT("Quality");
				Preset.Description = TEXT("Свет, Lumen, тени, полное разрешение. Медленно, зато как задумано.");
				Preset.bLit = true;
				Preset.bShowBackground = true;
				Preset.bCellsCastShadows = true;
				Preset.bCellCullingEnabled = false;
				Preset.ConsoleCommands = BuildCommands(FRenderCvars());
				Result.Add(MoveTemp(Preset));
			}

			// --- F2: Unlit ---------------------------------------------------
			// Отличается от Quality РОВНО одним: viewmode. Специально: это
			// "посмотреть без света", а не ступень оптимизации - цвет клетки
			// виден как есть, без вклада освещения, и сравнивать две картинки
			// имеет смысл только когда всё остальное совпадает.
			{
				FRenderPreset Preset;
				Preset.Name = TEXT("Unlit");
				Preset.Description = TEXT("То же, что Quality, но без освещения - чистый цвет клеток.");
				Preset.bLit = false;
				Preset.bShowBackground = true;
				Preset.bCellsCastShadows = true;
				Preset.bCellCullingEnabled = false;
				Preset.ConsoleCommands = BuildCommands(FRenderCvars());
				Result.Add(MoveTemp(Preset));
			}

			// --- F3: Balanced ------------------------------------------------
			// Всё, что стоит дорого и почти не видно на решётке из кубиков:
			// Lumen, тени (и от источников, и от самих клеток), пост-обработка,
			// фон. Разрешение при этом полное - картинка остаётся чёткой, и
			// клетки все на месте, поэтому это разумный "рабочий" профиль.
			{
				FRenderCvars Cvars;
				Cvars.LumenGlobalIllumination = 0;
				Cvars.LumenReflections = 0;
				Cvars.VirtualShadowMaps = 0;
				Cvars.ShadowQuality = 0;
				Cvars.AntiAliasing = 1; // FXAA - дёшево и без размазывания в движении
				Cvars.Bloom = 0;
				Cvars.DepthOfField = 0;
				Cvars.MotionBlur = 0;
				Cvars.AmbientOcclusion = 0;
				Cvars.VolumetricFog = 0;
				Cvars.ViewDistanceScale = 1;

				FRenderPreset Preset;
				Preset.Name = TEXT("Balanced");
				Preset.Description = TEXT("Без Lumen, теней, пост-обработки и фона. Полное разрешение, все клетки на месте.");
				Preset.bLit = false;
				Preset.bShowBackground = false;
				Preset.bCellsCastShadows = false;
				Preset.bCellCullingEnabled = true;
				Preset.CellCullStartDistance = 0.0f;
				Preset.CellCullEndDistance = 200000.0f;
				Preset.ConsoleCommands = BuildCommands(Cvars);
				Result.Add(MoveTemp(Preset));
			}

			// --- F4: Performance ---------------------------------------------
			// Всё из Balanced, плюс две вещи, которые уже меняют то, ЧТО видно,
			// а не только то, как это нарисовано: пониженное разрешение и Ghost
			// Shape. Последний - главный выигрыш профиля: без куба отсечения он
			// заменяет детальный рендер силуэтом по чанкам, т.е. AddInstances()
			// перестаёт получать миллионы инстансов (см.
			// ShouldGhostShapeReplaceDetailedRender()). Отдельных клеток при
			// этом не видно - это осознанная цена за "просто покажи форму".
			{
				FRenderCvars Cvars;
				Cvars.LumenGlobalIllumination = 0;
				Cvars.LumenReflections = 0;
				Cvars.VirtualShadowMaps = 0;
				Cvars.ShadowQuality = 0;
				Cvars.AntiAliasing = 0;
				Cvars.ScreenPercentage = 70;
				Cvars.Bloom = 0;
				Cvars.DepthOfField = 0;
				Cvars.MotionBlur = 0;
				Cvars.AmbientOcclusion = 0;
				Cvars.VolumetricFog = 0;
				Cvars.ViewDistanceScale = 1;

				FRenderPreset Preset;
				Preset.Name = TEXT("Performance");
				Preset.Description = TEXT("Всё из Balanced + разрешение 70% + силуэт по чанкам вместо отдельных клеток.");
				Preset.bLit = false;
				Preset.bShowBackground = false;
				Preset.bCellsCastShadows = false;
				Preset.bCellCullingEnabled = true;
				Preset.CellCullStartDistance = 0.0f;
				Preset.CellCullEndDistance = 80000.0f;
				Preset.bGhostShapeEnabled = true;
				Preset.ConsoleCommands = BuildCommands(Cvars);
				Result.Add(MoveTemp(Preset));
			}

			// --- Photo -------------------------------------------------------
			// Единственный профиль без своей F-клавиши: его применяет сама
			// съёмка (TakePhotoShot()), потому что вне снимка в нём нет смысла -
			// он заведомо медленнее Quality и нужен ровно на один кадр.
			//
			// От Quality не отличается ни одним cvar'ом, и это осознанно.
			//
			// Первая версия ставила сюда FXAA и ScreenPercentage 200 - против
			// швов между тайлами. Обоснование было ложным: HighResShot НЕ
			// тайлит, он создаёт один рендер-таргет запрошенного размера
			// (UnrealClient.cpp: DummyViewport->SizeX = GScreenshotResolutionX).
			// Швов не бывает, бороться не с чем - а ScreenPercentage 200 при
			// этом множил и без того огромный таргет на четыре по площади и
			// ронял редактор по нехватке видеопамяти (проверено: 7680x4320 при
			// 200% это 15360x8640, 133 мегапикселя GBuffer'а, и всего 82 тысячи
			// живых клеток в сцене - дело было не в сетке вовсе).
			//
			// Раз тайлов нет, временное сглаживание безопасно и желательно:
			// TSR из Quality остаётся, а сойтись ему даёт время задержка
			// затвора (PhotoShotDelayFrames -> r.HighResScreenshotDelay) при
			// неподвижной камере.
			//
			// ViewDistanceScale тоже как у Quality (10), а не 1: единица
			// заставила бы отсекать по расстоянию РАНЬШЕ, а весь смысл этого
			// профиля в том, чтобы в кадр попало всё.
			//
			// Профиль остаётся отдельной записью, хотя cvar'ы совпадают с
			// Quality: он гасит Ghost Shape и отсечение, которые Quality не
			// трогает, и главное - им владеет съёмка, так что его можно
			// настраивать под снимок, не задевая рабочий профиль.
			{
				FRenderCvars Cvars;

				FRenderPreset Preset;
				Preset.Name = TEXT("Photo");
				Preset.Description = TEXT("Всё включено, ничего не отсекается. Для снимка, не для работы.");
				Preset.bLit = true;
				Preset.bShowBackground = true;
				Preset.bCellsCastShadows = true;
				Preset.bCellCullingEnabled = false;
				Preset.bGhostShapeEnabled = false;
				Preset.ConsoleCommands = BuildCommands(Cvars);
				Result.Add(MoveTemp(Preset));
			}

			// --- Photo Lean --------------------------------------------------
			// Тот же снимок, но без подсистем, чей расход памяти растёт вместе с
			// размером кадра. На снимке это решает: кадр рендерится целиком, в
			// один таргет, и на 33 мегапикселях каждый полноразмерный буфер
			// стоит сотни мегабайт.
			//
			// Убрано именно то, что аллоцируется ПО РАЗМЕРУ КАДРА: Lumen
			// (экранные пробы и кэш радиантности), Virtual Shadow Maps (пул
			// страниц), история TSR, цепочки bloom/DOF/SSAO/тумана.
			//
			// А вот ShadowQuality остаётся высоким, и это не оплошность: с
			// выключенным VSM тени рисуются обычными картами, а те живут в своём
			// атласе фиксированного размера и от разрешения кадра не зависят.
			// Тени - половина того, ради чего снимок вообще делается, и терять
			// их вместе с памятью незачем.
			//
			// Сглаживание - FXAA вместо TSR: у TSR несколько полноразмерных
			// историй, а сходиться ему на неподвижном кадре всё равно негде
			// взять движения. Разрешение остаётся полным - его режет
			// PhotoShotResolution, а не профиль.
			{
				FRenderCvars Cvars;
				Cvars.LumenGlobalIllumination = 0;
				Cvars.LumenReflections = 0;
				Cvars.VirtualShadowMaps = 0;
				Cvars.AntiAliasing = 1;
				Cvars.Bloom = 0;
				Cvars.DepthOfField = 0;
				Cvars.MotionBlur = 0;
				Cvars.AmbientOcclusion = 0;
				Cvars.VolumetricFog = 0;

				FRenderPreset Preset;
				Preset.Name = TEXT("Photo Lean");
				Preset.Description = TEXT("Снимок без Lumen, VSM и пост-обработки - тени и свет на месте, видеопамяти в разы меньше.");
				Preset.bLit = true;
				Preset.bShowBackground = true;
				Preset.bCellsCastShadows = true;
				Preset.bCellCullingEnabled = false;
				Preset.bGhostShapeEnabled = false;
				Preset.ConsoleCommands = BuildCommands(Cvars);
				Result.Add(MoveTemp(Preset));
			}

			return Result;
		}();

		return Presets;
	}

	int32 GetPhotoPresetIndex(bool bLeanMemory)
	{
		const TCHAR* WantedName = bLeanMemory ? TEXT("Photo Lean") : TEXT("Photo");

		const TArray<FRenderPreset>& Presets = GetAll();
		for (int32 Index = 0; Index < Presets.Num(); ++Index)
		{
			if (Presets[Index].Name == WantedName)
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}
}