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
		if (NeighborhoodToken.Equals(TEXT("M"), ESearchCase::IgnoreCase) || NeighborhoodToken.Equals(TEXT("Moore"), ESearchCase::IgnoreCase))
		{
			Parsed.Neighborhood = ENeighborhood::Moore;
		}
		else if (NeighborhoodToken.Equals(TEXT("VN"), ESearchCase::IgnoreCase) || NeighborhoodToken.Equals(TEXT("VonNeumann"), ESearchCase::IgnoreCase))
		{
			Parsed.Neighborhood = ENeighborhood::VonNeumann;
		}
		else
		{
			OutError = FString::Printf(TEXT("Neighborhood: '%s' - ожидается 'M'/'Moore' или 'VN'/'VonNeumann'"), *NeighborhoodToken);
			return false;
		}

		OutResult = MoveTemp(Parsed);
		return true;
	}
}
