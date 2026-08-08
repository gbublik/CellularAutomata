#include "Automata/Simulation/RuleStringParser.h"

namespace
{
	/** true только для непустой строки из одних цифр (0-9) - счётчики
	 *  соседей всегда неотрицательны, никаких знаков/точек не ожидается.
	 *  Строже, чем FString::IsNumeric() (та пропускает "-"/"."). */
	bool IsNonNegativeIntegerToken(const FString& Token)
	{
		if (Token.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Char : Token)
		{
			if (!FChar::IsDigit(Char))
			{
				return false;
			}
		}
		return true;
	}

	/** Разбирает поле-список ("0-6" или "13-14,17-19" и т.п.) в плоский
	 *  список счётчиков соседей - каждый элемент через запятую либо одно
	 *  число, либо включительный диапазон "x-y" (x<=y), раскрытый в
	 *  отдельные значения. FieldName используется только для текста ошибки. */
	bool ParseCountListField(const FString& Field, const TCHAR* FieldName, TArray<int32>& OutCounts, FString& OutError)
	{
		TArray<FString> Tokens;
		Field.ParseIntoArray(Tokens, TEXT(","), /*InCullEmpty=*/true);

		if (Tokens.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s: поле не может быть пустым"), FieldName);
			return false;
		}

		for (FString Token : Tokens)
		{
			Token.TrimStartAndEndInline();

			FString LowStr, HighStr;
			if (Token.Split(TEXT("-"), &LowStr, &HighStr))
			{
				if (!IsNonNegativeIntegerToken(LowStr) || !IsNonNegativeIntegerToken(HighStr))
				{
					OutError = FString::Printf(TEXT("%s: '%s' должно быть числом или диапазоном 'x-y'"), FieldName, *Token);
					return false;
				}

				const int32 Low = FCString::Atoi(*LowStr);
				const int32 High = FCString::Atoi(*HighStr);
				if (Low > High)
				{
					OutError = FString::Printf(TEXT("%s: диапазон '%s' должен быть по возрастанию (x<=y)"), FieldName, *Token);
					return false;
				}

				for (int32 Count = Low; Count <= High; ++Count)
				{
					OutCounts.Add(Count);
				}
			}
			else
			{
				if (!IsNonNegativeIntegerToken(Token))
				{
					OutError = FString::Printf(TEXT("%s: '%s' должно быть числом или диапазоном 'x-y'"), FieldName, *Token);
					return false;
				}

				OutCounts.Add(FCString::Atoi(*Token));
			}
		}

		return true;
	}

	/** Таблица токенов окрестности: короткая и длинная форма для каждого
	 *  значения, регистронезависимо. Одна таблица служит обеим сторонам -
	 *  разбору и печати, - чтобы они не могли разъехаться.
	 *
	 *  Токен "VN2" заканчивается цифрой, и это теперь нормально: пока
	 *  существовал отдельный радиус, хвостовые цифры отрезались как его
	 *  значение, и такой токен разобрался бы как "VN" радиуса 2. Радиуса
	 *  больше нет (см. Neighborhood.h), так что имя сравнивается целиком. */
	struct FNeighborhoodToken
	{
		const TCHAR* Short;
		const TCHAR* Long;
		ENeighborhood Value;
	};

	static const FNeighborhoodToken NeighborhoodTokens[] = {
		{ TEXT("VN"),   TEXT("VonNeumann"),          ENeighborhood::VonNeumann },
		{ TEXT("M"),    TEXT("Moore"),               ENeighborhood::Moore },
		{ TEXT("VN2"),  TEXT("VonNeumann2"),         ENeighborhood::VonNeumann2 },
		{ TEXT("E"),    TEXT("Edges"),               ENeighborhood::Edges },
		{ TEXT("C"),    TEXT("Corners"),             ENeighborhood::Corners },
		{ TEXT("FA"),   TEXT("FarAxes"),             ENeighborhood::FarAxes },
		{ TEXT("FE"),   TEXT("FacesEdges"),          ENeighborhood::FacesEdges },
		{ TEXT("FC"),   TEXT("FacesCorners"),        ENeighborhood::FacesCorners },
		{ TEXT("FFA"),  TEXT("FacesFarAxes"),        ENeighborhood::FacesFarAxes },
		{ TEXT("EC"),   TEXT("EdgesCorners"),        ENeighborhood::EdgesCorners },
		{ TEXT("EFA"),  TEXT("EdgesFarAxes"),        ENeighborhood::EdgesFarAxes },
		{ TEXT("CFA"),  TEXT("CornersFarAxes"),      ENeighborhood::CornersFarAxes },
		{ TEXT("FCFA"), TEXT("FacesCornersFarAxes"), ENeighborhood::FacesCornersFarAxes },
		{ TEXT("ECFA"), TEXT("EdgesCornersFarAxes"), ENeighborhood::EdgesCornersFarAxes },
		// Анизотропное: Moore в плоскости XY плюс ось Z - см. ENeighborhood.
		{ TEXT("PM"),   TEXT("PlanarMoore"),         ENeighborhood::PlanarMoore },
	};

	bool ParseNeighborhoodName(const FString& Name, ENeighborhood& OutNeighborhood)
	{
		for (const FNeighborhoodToken& Token : NeighborhoodTokens)
		{
			if (Name.Equals(Token.Short, ESearchCase::IgnoreCase) || Name.Equals(Token.Long, ESearchCase::IgnoreCase))
			{
				OutNeighborhood = Token.Value;
				return true;
			}
		}
		return false;
	}

	/** Обратное к ParseNeighborhoodName(): короткая форма, та, что печатается
	 *  в строку правила. */
	const TCHAR* FormatNeighborhoodName(ENeighborhood Neighborhood)
	{
		for (const FNeighborhoodToken& Token : NeighborhoodTokens)
		{
			if (Token.Value == Neighborhood)
			{
				return Token.Short;
			}
		}
		return TEXT("M");
	}

	/** Перечень допустимых токенов одной строкой - для текста ошибки, чтобы он
	 *  не разъехался с таблицей выше при добавлении окрестности. */
	FString DescribeNeighborhoodTokens()
	{
		FString Result;
		for (const FNeighborhoodToken& Token : NeighborhoodTokens)
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT(", ");
			}
			Result += FString::Printf(TEXT("'%s'/'%s'"), Token.Short, Token.Long);
		}
		return Result;
	}

	/** Сжимает список счётчиков в поле строки правила: сортирует, убирает
	 *  повторы и склеивает подряд идущие значения в диапазоны "x-y" - ровно
	 *  та форма, которую понимает ParseCountListField() выше. Пустой список
	 *  даёт пустое поле (сама по себе такая строка не разберётся обратно, но
	 *  и правило с пустым Birth/Survival невалидно - показать его лучше как
	 *  есть, чем подставить что-то несуществующее). */
	FString FormatCountListField(const TArray<int32>& Counts)
	{
		TArray<int32> Sorted = Counts;
		Sorted.Sort();

		FString Result;
		for (int32 Index = 0; Index < Sorted.Num(); )
		{
			// Хвост одного диапазона: пропускаем повторы (Next == Last) и
			// шаг на единицу (Next == Last + 1).
			const int32 RangeStart = Sorted[Index];
			int32 RangeEnd = RangeStart;
			while (++Index < Sorted.Num() && Sorted[Index] <= RangeEnd + 1)
			{
				RangeEnd = Sorted[Index];
			}

			if (!Result.IsEmpty())
			{
				Result += TEXT(",");
			}
			Result += (RangeStart == RangeEnd)
				? FString::FromInt(RangeStart)
				: FString::Printf(TEXT("%d-%d"), RangeStart, RangeEnd);
		}

		return Result;
	}
}

namespace RuleStringParser
{
	bool ParseRuleString(const FString& RuleString, FParsedRule& OutResult, FString& OutError)
	{
		TArray<FString> Fields;
		RuleString.ParseIntoArray(Fields, TEXT("/"), /*InCullEmpty=*/false);

		if (Fields.Num() != 4)
		{
			OutError = FString::Printf(TEXT("Ожидается формат 'Survival/Birth/States/Neighborhood' (например '0-6/1,3/2/VN'), получено %d поле(й) вместо 4"), Fields.Num());
			return false;
		}

		FParsedRule Parsed;

		if (!ParseCountListField(Fields[0], TEXT("Survival"), Parsed.SurvivalCounts, OutError))
		{
			return false;
		}
		if (!ParseCountListField(Fields[1], TEXT("Birth"), Parsed.BirthCounts, OutError))
		{
			return false;
		}

		FString StatesToken = Fields[2];
		StatesToken.TrimStartAndEndInline();
		if (!IsNonNegativeIntegerToken(StatesToken))
		{
			OutError = FString::Printf(TEXT("States: '%s' должно быть целым числом >= 2"), *StatesToken);
			return false;
		}
		const int32 ParsedStates = FCString::Atoi(*StatesToken);
		if (ParsedStates < 2)
		{
			OutError = FString::Printf(TEXT("States: %d - должно быть >= 2"), ParsedStates);
			return false;
		}
		Parsed.States = ParsedStates;

		FString NeighborhoodToken = Fields[3];
		NeighborhoodToken.TrimStartAndEndInline();

		if (!ParseNeighborhoodName(NeighborhoodToken, Parsed.Neighborhood))
		{
			OutError = FString::Printf(TEXT("Neighborhood: '%s' - ожидается одно из: %s"), *NeighborhoodToken, *DescribeNeighborhoodTokens());
			return false;
		}

		OutResult = MoveTemp(Parsed);
		return true;
	}

	FString FormatRuleString(const TArray<int32>& SurvivalCounts, const TArray<int32>& BirthCounts, int32 States, ENeighborhood Neighborhood)
	{
		// Порядок полей - Survival, затем Birth: тот же, что читает
		// ParseRuleString(), и обратный порядку объявления BirthCounts/
		// SurvivalCounts в AAutomataOrchestrator (см. предупреждение в
		// doc-comment'е ApplyRuleString() - здесь та же ловушка, поэтому
		// параметры функции идут именно в порядке строки, а не полей актора).
		return FString::Printf(TEXT("%s/%s/%d/%s"),
			*FormatCountListField(SurvivalCounts),
			*FormatCountListField(BirthCounts),
			States,
			FormatNeighborhoodName(Neighborhood));
	}
}
