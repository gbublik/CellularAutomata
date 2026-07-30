#include "Automata/Capture/CapturePresets.h"

namespace CapturePresets
{
	namespace
	{
		FCapturePreset MakePreset(const TCHAR* Name, const TCHAR* Description, const FSliceCaptureParams& Params)
		{
			FCapturePreset Preset;
			Preset.Name = Name;
			Preset.Description = Description;
			Preset.Params = Params;
			return Preset;
		}
	}

	const TArray<FCapturePreset>& GetAll()
	{
		// Индекс 0 - дорога назад, как профиль Quality у RenderPresets: пресеты
		// перезаписывают ВСЕ поля, и без набора со значениями по умолчанию
		// вернуться к обычной съёмке можно было бы только перебором полей в
		// панели по памяти.
		//
		// Дальше - три набора под поиск логотипа, и они разделены именно так,
		// потому что это три разных вопроса к картинке, а не три вкуса одного:
		//
		//   1. "Читается ли форма вообще" - силуэт. Цвет, рампа и возрасты тут
		//      не участвуют вовсе, и это к лучшему: знак должен узнаваться
		//      одним тоном, иначе он держится на градиенте и рассыплется в
		//      мелком размере или в печати. Один пиксель на клетку - потому что
		//      это и есть проверка на мелкий размер; крупный масштаб на этом
		//      этапе только маскирует проблему.
		//
		//   2. "А если симметрично" - то же самое зеркальной плиткой. Симметрия
		//      почти всегда читается как знак, а не как пятно, и достаётся
		//      бесплатно: отражение стыкуется по построению (см. ESliceTileMode).
		//      Цена - результат обязательно симметричен, поэтому это отдельный
		//      заход, а не замена первому.
		//
		//   3. "Победитель крупно" - полноцветный снимок выбранного кадра. Один
		//      кадр вместо серии: искать уже нечего.
		//
		// Длина серии у переборных наборов заметно больше стандартной, а шаг в
		// поколениях - крупнее. Соседние поколения похожи друг на друга почти до
		// неразличимости, так что мелкий шаг даёт не больше кандидатов, а те же
		// самые кандидаты в нескольких экземплярах.

		static const TArray<FCapturePreset> Presets = {
			MakePreset(
				TEXT("Обычная съёмка"),
				TEXT("Значения по умолчанию - дорога назад из любого набора."),
				[]
				{
					// Именно default-конструкция, а не переписанные вручную
					// значения: иначе "по умолчанию" здесь и в
					// FSliceCaptureParams разъехались бы при первой же правке
					// умолчаний в структуре, причём молча.
					return FSliceCaptureParams{};
				}()),

			MakePreset(
				TEXT("Лого: силуэт"),
				TEXT("Перебор кандидатов: двухцветный силуэт, 1 пиксель на клетку, длинная серия. Проверка на то, читается ли форма без цвета и в мелком размере."),
				[]
				{
					FSliceCaptureParams P;
					P.Mode = ECellRasterMode::Silhouette;
					P.PixelsPerCell = 1;
					P.TileMode = ESliceTileMode::None;
					P.bTransparentBackground = false;
					P.BackgroundColor = FColor(0, 0, 0, 255);
					P.ForegroundColor = FColor::White;
					P.bEncodeSRGB = true;
					P.SeriesFrameCount = 60;
					P.SeriesGenerationsPerFrame = 10;
					P.bSeriesFastMode = true;
					return P;
				}()),

			MakePreset(
				TEXT("Лого: орнамент"),
				TEXT("То же, но зеркальной плиткой по обеим осям - симметричный знак. Стыкуется без швов по построению."),
				[]
				{
					FSliceCaptureParams P;
					P.Mode = ECellRasterMode::Silhouette;
					P.PixelsPerCell = 1;
					P.TileMode = ESliceTileMode::MirrorBoth;
					P.bTransparentBackground = false;
					P.BackgroundColor = FColor(0, 0, 0, 255);
					P.ForegroundColor = FColor::White;
					P.bEncodeSRGB = true;
					P.SeriesFrameCount = 60;
					P.SeriesGenerationsPerFrame = 10;
					P.bSeriesFastMode = true;
					return P;
				}()),

			MakePreset(
				TEXT("Лого: финал"),
				TEXT("Экспорт выбранного кадра: полный цвет, 8 пикселей на клетку, прозрачный фон, один кадр. Если победил орнамент - верните TileMode = MirrorBoth."),
				[]
				{
					FSliceCaptureParams P;
					// Полный цвет, а не силуэт: перебор идёт по форме, а кладут
					// в макет всё-таки картинку с рампой - ровно ту, что видно
					// на экране.
					P.Mode = ECellRasterMode::NearestToCamera;
					// Масштабирование здесь - копирование блоками, а не
					// фильтрация, поэтому крупный снимок остаётся пиксельно
					// чётким и не нуждается в пересохранении из редактора.
					P.PixelsPerCell = 8;
					// Плитка сброшена намеренно, хотя после "орнамента" её,
					// возможно, захотят вернуть: набор, который что-то НЕ
					// задаёт, зависел бы от того, откуда в него пришли, - см.
					// doc-comment FCapturePreset. Это одно поле в панели.
					P.TileMode = ESliceTileMode::None;
					// Знак кладут поверх чего-то - прозрачный фон снимает
					// обтравку.
					P.bTransparentBackground = true;
					P.ForegroundColor = FColor::White;
					P.bEncodeSRGB = true;
					// Серия из одного кадра: это набор под F6, а не под F7.
					P.SeriesFrameCount = 1;
					P.SeriesGenerationsPerFrame = 10;
					P.bSeriesFastMode = true;
					return P;
				}()),
		};

		return Presets;
	}
}