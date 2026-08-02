#include "Automata/Rendering/ColorRamp.h"

namespace
{
	/** Возведение в степень с сохранением знака. Нужно везде, где применяется
	 *  нелинейная передаточная функция: FMath::Pow от отрицательного числа даёт
	 *  NaN, а отрицательные компоненты в FLinearColor вполне возможны - и как
	 *  результат вылета сплайна, и просто как заданное значение. */
	FORCEINLINE float SignedPow(float X, float Exponent)
	{
		const float Magnitude = FMath::Pow(FMath::Abs(X), Exponent);
		return (X >= 0.0f) ? Magnitude : -Magnitude;
	}

	FORCEINLINE float SrgbEncode(float X)
	{
		const float A = FMath::Abs(X);
		const float Encoded = (A <= 0.0031308f) ? (A * 12.92f) : (1.055f * FMath::Pow(A, 1.0f / 2.4f) - 0.055f);
		return (X >= 0.0f) ? Encoded : -Encoded;
	}

	FORCEINLINE float SrgbDecode(float X)
	{
		const float A = FMath::Abs(X);
		const float Decoded = (A <= 0.04045f) ? (A / 12.92f) : FMath::Pow((A + 0.055f) / 1.055f, 2.4f);
		return (X >= 0.0f) ? Decoded : -Decoded;
	}

	/** Линейный sRGB -> OKLab. Матрицы и порядок - по оригинальной публикации
	 *  Björn Ottosson (2020); трогать числа по одному нельзя, они парные с
	 *  обратным преобразованием ниже. */
	FVector3f LinearToOklab(float R, float G, float B)
	{
		const float L = 0.4122214708f * R + 0.5363325363f * G + 0.0514459929f * B;
		const float M = 0.2119034982f * R + 0.6806995451f * G + 0.1073969566f * B;
		const float S = 0.0883024619f * R + 0.2817188376f * G + 0.6299787005f * B;

		// Кубический корень со знаком - см. SignedPow: до клампа компоненты
		// могут быть отрицательными, а cbrt отрицательного вполне определён.
		const float L_ = SignedPow(L, 1.0f / 3.0f);
		const float M_ = SignedPow(M, 1.0f / 3.0f);
		const float S_ = SignedPow(S, 1.0f / 3.0f);

		return FVector3f(
			0.2104542553f * L_ + 0.7936177850f * M_ - 0.0040720468f * S_,
			1.9779984951f * L_ - 2.4285922050f * M_ + 0.4505937099f * S_,
			0.0259040371f * L_ + 0.7827717662f * M_ - 0.8086757660f * S_);
	}

	/** OKLab -> линейный sRGB. */
	FVector3f OklabToLinear(const FVector3f& Lab)
	{
		const float L_ = Lab.X + 0.3963377774f * Lab.Y + 0.2158037573f * Lab.Z;
		const float M_ = Lab.X - 0.1055613458f * Lab.Y - 0.0638541728f * Lab.Z;
		const float S_ = Lab.X - 0.0894841775f * Lab.Y - 1.2914855480f * Lab.Z;

		const float L = L_ * L_ * L_;
		const float M = M_ * M_ * M_;
		const float S = S_ * S_ * S_;

		return FVector3f(
			 4.0767416621f * L - 3.3077115913f * M + 0.2309699292f * S,
			-1.2684380046f * L + 2.6097574011f * M - 0.3413193965f * S,
			-0.0041960863f * L - 0.7034186147f * M + 1.7076147010f * S);
	}

	/** Ниже цвет живёт как FVector4f: XYZ - координаты в рабочем пространстве,
	 *  W - альфа. Альфа во всех пространствах интерполируется одинаково, ей
	 *  никакое преобразование не нужно. */
	FVector4f ToWorkingSpace(const FLinearColor& Color, EColorRampSpace Space)
	{
		switch (Space)
		{
		case EColorRampSpace::Srgb:
			return FVector4f(SrgbEncode(Color.R), SrgbEncode(Color.G), SrgbEncode(Color.B), Color.A);

		case EColorRampSpace::Oklab:
		{
			const FVector3f Lab = LinearToOklab(Color.R, Color.G, Color.B);
			return FVector4f(Lab.X, Lab.Y, Lab.Z, Color.A);
		}

		case EColorRampSpace::Oklch:
		{
			const FVector3f Lab = LinearToOklab(Color.R, Color.G, Color.B);
			const float Chroma = FMath::Sqrt(Lab.Y * Lab.Y + Lab.Z * Lab.Z);
			const float Hue = FMath::Atan2(Lab.Z, Lab.Y);
			return FVector4f(Lab.X, Chroma, Hue, Color.A);
		}

		case EColorRampSpace::LinearRgb:
		default:
			return FVector4f(Color.R, Color.G, Color.B, Color.A);
		}
	}

	FLinearColor FromWorkingSpace(const FVector4f& Value, EColorRampSpace Space)
	{
		FVector3f Linear;
		switch (Space)
		{
		case EColorRampSpace::Srgb:
			Linear = FVector3f(SrgbDecode(Value.X), SrgbDecode(Value.Y), SrgbDecode(Value.Z));
			break;

		case EColorRampSpace::Oklab:
			Linear = OklabToLinear(FVector3f(Value.X, Value.Y, Value.Z));
			break;

		case EColorRampSpace::Oklch:
		{
			// Обратно из полярной формы: насыщенность зажимается снизу нулём,
			// потому что сплайн умеет увести её в минус, а отрицательная
			// насыщенность означает разворот тона на 180 градусов - то есть
			// цвет, которого в рампе никто не задавал.
			const float Chroma = FMath::Max(Value.Y, 0.0f);
			const FVector3f Lab(Value.X, Chroma * FMath::Cos(Value.Z), Chroma * FMath::Sin(Value.Z));
			Linear = OklabToLinear(Lab);
			break;
		}

		case EColorRampSpace::LinearRgb:
		default:
			Linear = FVector3f(Value.X, Value.Y, Value.Z);
			break;
		}

		// Зажим обязателен, а не защитный: Катмулл-Ром вылетает за диапазон
		// опорных значений по построению, а преобразования из OKLab к тому же
		// умеют выдавать цвета вне гаммы. Диапазон [0,1] согласован с тем, что
		// цвет всё равно уезжает в PerInstanceCustomData как байт на канал, а
		// яркость выше единицы в этом проекте задаётся скалярным параметром на
		// Material Instance (см. FCellRenderInstance).
		return FLinearColor(
			FMath::Clamp(Linear.X, 0.0f, 1.0f),
			FMath::Clamp(Linear.Y, 0.0f, 1.0f),
			FMath::Clamp(Linear.Z, 0.0f, 1.0f),
			FMath::Clamp(Value.W, 0.0f, 1.0f));
	}

	/** Разворачивает последовательность углов тона так, чтобы соседние точки
	 *  отличались не больше чем на половину оборота. Без этого переход,
	 *  например, с 350 на 10 градусов пошёл бы «длинным путём» через весь круг.
	 *
	 *  Делается по контрольным точкам, а не по всему списку ключей: и линейной
	 *  интерполяции, и сплайну нужны только соседние, а локального разворота
	 *  для короткой дуги достаточно. */
	void UnwrapHues(TArrayView<FVector4f> Points)
	{
		// У серого и чёрного тон не определён (насыщенность нулевая), и atan2
		// вернул бы для них произвольное значение. Такие точки берут тон
		// соседа - сначала ищем первую осмысленную и тянем её назад.
		constexpr float ChromaEpsilon = 1e-5f;
		int32 FirstMeaningful = INDEX_NONE;
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			if (Points[Index].Y > ChromaEpsilon)
			{
				FirstMeaningful = Index;
				break;
			}
		}

		if (FirstMeaningful == INDEX_NONE)
		{
			// Вся четвёрка серая - тон не важен, интерполировать нечего.
			return;
		}

		for (int32 Index = FirstMeaningful - 1; Index >= 0; --Index)
		{
			Points[Index].Z = Points[FirstMeaningful].Z;
		}

		for (int32 Index = FirstMeaningful + 1; Index < Points.Num(); ++Index)
		{
			if (Points[Index].Y <= ChromaEpsilon)
			{
				Points[Index].Z = Points[Index - 1].Z;
				continue;
			}

			const float Delta = FMath::UnwindRadians(Points[Index].Z - Points[Index - 1].Z);
			Points[Index].Z = Points[Index - 1].Z + Delta;
		}
	}

	/** Равномерный Катмулл-Ром по четырём контрольным точкам, T в [0,1] между
	 *  P1 и P2. Кривая проходит ровно через P1 и P2 - на этом держится
	 *  гарантия «в опорных точках получается сам ключ». */
	FVector4f CatmullRom(const FVector4f& P0, const FVector4f& P1,
						 const FVector4f& P2, const FVector4f& P3, float T)
	{
		const float T2 = T * T;
		const float T3 = T2 * T;

		return 0.5f * ((2.0f * P1)
			+ (-P0 + P2) * T
			+ (2.0f * P0 - 5.0f * P1 + 4.0f * P2 - P3) * T2
			+ (-P0 + 3.0f * P1 - 3.0f * P2 + P3) * T3);
	}
}

FLinearColor ColorRamp::Sample(const TArray<FLinearColor>& Keys, float T,
							   EColorRampSpace Space, EColorRampCurve Curve)
{
	if (Keys.Num() == 0)
	{
		// Белый, а не отказ рисовать: пустая рампа - нормальное промежуточное
		// состояние настройки, и белый означает "как выглядит сам материал"
		// (см. doc-comment AgeColors).
		return FLinearColor::White;
	}
	if (Keys.Num() == 1)
	{
		return Keys[0];
	}

	const float Position = FMath::Clamp(T, 0.0f, 1.0f) * float(Keys.Num() - 1);
	const int32 LowIndex = FMath::Clamp(FMath::FloorToInt(Position), 0, Keys.Num() - 2);
	const float LocalT = Position - float(LowIndex);

	// Контрольные точки. Для линейной хватило бы двух, но собираем четвёрку
	// всегда: развороту тона (UnwrapHues) нужна цепочка целиком, а не пара, и
	// расхождение между ветками здесь стоило бы тонкого расхождения тона между
	// Linear и CatmullRom на одних и тех же ключах.
	const int32 Indices[4] = {
		FMath::Max(LowIndex - 1, 0),
		LowIndex,
		LowIndex + 1,
		FMath::Min(LowIndex + 2, Keys.Num() - 1)
	};

	TArray<FVector4f, TInlineAllocator<4>> Points;
	Points.Reserve(4);
	for (int32 Index : Indices)
	{
		Points.Add(ToWorkingSpace(Keys[Index], Space));
	}

	if (Space == EColorRampSpace::Oklch)
	{
		UnwrapHues(Points);
	}

	const FVector4f Result = (Curve == EColorRampCurve::CatmullRom)
		? CatmullRom(Points[0], Points[1], Points[2], Points[3], LocalT)
		: FMath::Lerp(Points[1], Points[2], LocalT);

	return FromWorkingSpace(Result, Space);
}
