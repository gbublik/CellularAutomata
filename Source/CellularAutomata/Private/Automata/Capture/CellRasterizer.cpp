#include "Automata/Capture/CellRasterizer.h"

namespace
{
	/** Целочисленные координаты клетки в системе снимка: X вправо, Y вниз,
	 *  Z - глубина от камеры. Оси осевые, а позиции кратны CellSize, поэтому
	 *  деление даёт целое число, и округление здесь лишь снимает погрешность
	 *  double, а не выбирает между двумя пикселями. */
	struct FProjected
	{
		int32 X = 0;
		int32 Y = 0;
		int32 Depth = 0;
	};

	FORCEINLINE FProjected ProjectCell(const FVector3f& Position, const CellRasterizer::FRasterParams& Params)
	{
		const FVector World(Position);
		const double InvCellSize = 1.0 / FMath::Max(Params.CellSize, UE_DOUBLE_SMALL_NUMBER);

		FProjected Result;
		Result.X = FMath::RoundToInt(FVector::DotProduct(World, Params.RightAxis) * InvCellSize);
		// Минус: в изображении ось Y растёт вниз, а UpAxis смотрит вверх.
		Result.Y = -FMath::RoundToInt(FVector::DotProduct(World, Params.UpAxis) * InvCellSize);
		Result.Depth = FMath::RoundToInt(FVector::DotProduct(World, Params.ForwardAxis) * InvCellSize);
		return Result;
	}

	/** Габариты занятой области в клетках. Считаются по фактическим клеткам, а
	 *  не по границам сетки, поэтому пустых полей по краям не бывает и отдельная
	 *  обрезка не нужна. */
	bool ComputeCellBounds(const TArray<FCellRenderInstance>& Cells, const CellRasterizer::FRasterParams& Params,
						   FIntPoint& OutMin, FIntPoint& OutMax)
	{
		if (Cells.Num() == 0)
		{
			return false;
		}

		OutMin = FIntPoint(MAX_int32, MAX_int32);
		OutMax = FIntPoint(MIN_int32, MIN_int32);

		for (const FCellRenderInstance& Cell : Cells)
		{
			const FProjected P = ProjectCell(Cell.Position, Params);
			OutMin.X = FMath::Min(OutMin.X, P.X);
			OutMin.Y = FMath::Min(OutMin.Y, P.Y);
			OutMax.X = FMath::Max(OutMax.X, P.X);
			OutMax.Y = FMath::Max(OutMax.Y, P.Y);
		}

		return true;
	}
}

namespace CellRasterizer
{
	FVector SnapToAxis(const FVector& Direction)
	{
		const double AbsX = FMath::Abs(Direction.X);
		const double AbsY = FMath::Abs(Direction.Y);
		const double AbsZ = FMath::Abs(Direction.Z);

		// Вырожденный вектор: осей всё равно надо три штуки, и любая
		// определённость лучше NaN дальше по коду.
		if (AbsX <= UE_DOUBLE_SMALL_NUMBER && AbsY <= UE_DOUBLE_SMALL_NUMBER && AbsZ <= UE_DOUBLE_SMALL_NUMBER)
		{
			return FVector(1.0, 0.0, 0.0);
		}

		if (AbsX >= AbsY && AbsX >= AbsZ)
		{
			return FVector(Direction.X >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
		}

		if (AbsY >= AbsZ)
		{
			return FVector(0.0, Direction.Y >= 0.0 ? 1.0 : -1.0, 0.0);
		}

		return FVector(0.0, 0.0, Direction.Z >= 0.0 ? 1.0 : -1.0);
	}

	void BuildAxes(const FVector& CameraForward, const FVector& CameraUp, FRasterParams& OutParams)
	{
		OutParams.ForwardAxis = SnapToAxis(CameraForward);

		// Ось взгляда выбывает из кандидатов на "верх": иначе при диагональном
		// ракурсе обе округлились бы в одну ось и проекция схлопнулась бы.
		FVector UpCandidate = CameraUp;
		if (FMath::Abs(OutParams.ForwardAxis.X) > 0.5) { UpCandidate.X = 0.0; }
		else if (FMath::Abs(OutParams.ForwardAxis.Y) > 0.5) { UpCandidate.Y = 0.0; }
		else { UpCandidate.Z = 0.0; }

		// Камера смотрит ровно вдоль своего же "верха" - геометрически
		// невозможно, но вход сюда приходит снаружи, а вырожденные оси дальше
		// дали бы нулевое изображение вместо честной ошибки.
		if (UpCandidate.IsNearlyZero())
		{
			UpCandidate = (FMath::Abs(OutParams.ForwardAxis.Z) > 0.5) ? FVector(1.0, 0.0, 0.0) : FVector(0.0, 0.0, 1.0);
		}

		OutParams.UpAxis = SnapToAxis(UpCandidate);

		// Горизонталь не снапается, а выводится: две осевые ортогональные оси
		// дают третью автоматически, и промахнуться мимо ортогональности
		// становится невозможно. Порядок множителей - под левостороннюю
		// систему координат Unreal (X вперёд, Y вправо, Z вверх): при взгляде
		// вдоль +X и верхе +Z получается именно +Y.
		OutParams.RightAxis = FVector::CrossProduct(OutParams.UpAxis, OutParams.ForwardAxis);
	}

	bool ComputeImageSize(const TArray<FCellRenderInstance>& Cells, const FRasterParams& Params,
						  int32& OutWidth, int32& OutHeight)
	{
		FIntPoint Min;
		FIntPoint Max;
		if (!ComputeCellBounds(Cells, Params, Min, Max))
		{
			return false;
		}

		const int32 Scale = FMath::Max(Params.PixelsPerCell, 1);
		OutWidth = (Max.X - Min.X + 1) * Scale;
		OutHeight = (Max.Y - Min.Y + 1) * Scale;
		return true;
	}

	bool Rasterize(const TArray<FCellRenderInstance>& Cells, const FRasterParams& Params,
				   int64 MaxPixels, FRasterImage& OutImage, FString& OutError)
	{
		FIntPoint Min;
		FIntPoint Max;
		if (!ComputeCellBounds(Cells, Params, Min, Max))
		{
			OutError = TEXT("нечего снимать - ни одной видимой клетки");
			return false;
		}

		const int32 Scale = FMath::Max(Params.PixelsPerCell, 1);
		const int64 CellsWide = static_cast<int64>(Max.X) - Min.X + 1;
		const int64 CellsHigh = static_cast<int64>(Max.Y) - Min.Y + 1;
		const int64 Width = CellsWide * Scale;
		const int64 Height = CellsHigh * Scale;
		const int64 PixelCount = Width * Height;

		// Проверка ДО единой аллокации: превышение обязано быть отказом, а не
		// попыткой выделить гигабайты и упасть.
		if (PixelCount > MaxPixels)
		{
			OutError = FString::Printf(
				TEXT("снимок %lldx%lld это %lld пикселей при пределе %lld - уменьшите масштаб или отсеките область кубом"),
				Width, Height, PixelCount, MaxPixels);
			return false;
		}

		if (Width > MAX_int32 || Height > MAX_int32 || PixelCount > MAX_int32)
		{
			OutError = TEXT("снимок не помещается в 32-битную адресацию буфера");
			return false;
		}

		// Собираем во внутренние буферы и отдаём наружу только при успехе.
		const int32 WidthInt = static_cast<int32>(Width);
		const int32 HeightInt = static_cast<int32>(Height);
		const int32 CellsWideInt = static_cast<int32>(CellsWide);
		const int32 CellsHighInt = static_cast<int32>(CellsHigh);
		const int32 CellCount = CellsWideInt * CellsHighInt;

		// Свёртка считается в разрешении КЛЕТОК, а не пикселей: масштаб потом
		// просто размножает готовый цвет по блоку NxN. Иначе Z-буфер был бы в
		// N^2 раз больше без единого лишнего бита информации.
		TArray<FColor> CellColors;
		CellColors.Init(Params.BackgroundColor, CellCount);

		TArray<int32> CellDepth;
		TArray<int32> CellCounts;

		const bool bThickness = (Params.Mode == ECellRasterMode::Thickness);
		const bool bSilhouette = (Params.Mode == ECellRasterMode::Silhouette);
		if (bThickness)
		{
			CellCounts.Init(0, CellCount);
		}
		else if (!bSilhouette)
		{
			CellDepth.Init(MAX_int32, CellCount);
		}

		for (const FCellRenderInstance& Cell : Cells)
		{
			const FProjected P = ProjectCell(Cell.Position, Params);
			const int32 LocalX = P.X - Min.X;
			const int32 LocalY = P.Y - Min.Y;
			const int32 Index = LocalY * CellsWideInt + LocalX;

			if (bThickness)
			{
				++CellCounts[Index];
				continue;
			}

			if (bSilhouette)
			{
				// Ни глубина, ни цвет не нужны: важно только "здесь что-то
				// есть". Поэтому же режим не зависит от порядка клеток.
				CellColors[Index] = Params.ForegroundColor;
				continue;
			}

			// Строго меньше: при равной глубине побеждает первая встреченная,
			// поэтому результат не зависит от порядка клеток во входном списке
			// (две клетки не могут занимать одну ячейку решётки, так что
			// совпадение глубины означает разные пиксели).
			if (P.Depth < CellDepth[Index])
			{
				CellDepth[Index] = P.Depth;
				FColor Color = Cell.Color;
				// Клетки всегда непрозрачны - прозрачность зарезервирована за
				// фоном, иначе маска получилась бы дырявой.
				Color.A = 255;
				CellColors[Index] = Color;
			}
		}

		if (bThickness)
		{
			int32 MaxCount = 0;
			for (const int32 Count : CellCounts)
			{
				MaxCount = FMath::Max(MaxCount, Count);
			}

			// Нормируем в серый по самому толстому месту: абсолютные значения
			// зависят от размера структуры и как картинка бесполезны.
			const float InvMax = (MaxCount > 0) ? (1.0f / static_cast<float>(MaxCount)) : 0.0f;
			for (int32 Index = 0; Index < CellCount; ++Index)
			{
				if (CellCounts[Index] == 0)
				{
					continue;
				}

				const uint8 Level = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(CellCounts[Index] * InvMax * 255.0f), 1, 255));
				CellColors[Index] = FColor(Level, Level, Level, 255);
			}
		}

		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(static_cast<int32>(PixelCount));

		// Масштабирование - копирование, а не интерполяция: каждая клетка это
		// ровный блок одного цвета, границы попадают точно на пиксели.
		for (int32 CellY = 0; CellY < CellsHighInt; ++CellY)
		{
			for (int32 CellX = 0; CellX < CellsWideInt; ++CellX)
			{
				const FColor Color = CellColors[CellY * CellsWideInt + CellX];

				for (int32 SubY = 0; SubY < Scale; ++SubY)
				{
					const int32 RowStart = (CellY * Scale + SubY) * WidthInt + CellX * Scale;
					for (int32 SubX = 0; SubX < Scale; ++SubX)
					{
						Pixels[RowStart + SubX] = Color;
					}
				}
			}
		}

		OutImage.Width = WidthInt;
		OutImage.Height = HeightInt;
		OutImage.Pixels = MoveTemp(Pixels);
		return true;
	}
}
