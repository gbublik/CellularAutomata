#pragma once

#include "CoreMinimal.h"
#include "LightPresets.generated.h"

/** Готовый набор настроек света - солнце уровня плюс студийный риг.
 *
 *  USTRUCT(BlueprintType) с BlueprintReadOnly-полями - ровно тот же идиом, что
 *  у FRulePreset/FRenderPreset/FCapturePreset: таблицу читает UMG-виджет, а не
 *  только нативный код, и менять её из Blueprint нельзя (пресеты - константы
 *  кода, не состояние актора).
 *
 *  Пресет задаёт ВСЕ свои поля целиком, без исключений, - иначе переключение
 *  оставляло бы хвост от предыдущего, и "Студия" после "Контрового" выглядела
 *  бы не так, как "Студия" после "Солнца". Это общее правило всех четырёх
 *  таблиц проекта.
 *
 *  Чего пресет НЕ задаёт и почему: геометрия рига (откуда именно светят три
 *  источника) - константа кода, классическая трёхточечная схема, одна на все
 *  пресеты. Пресеты владеют СВЕТОМ, а не расстановкой: менять яркость и
 *  температуру осмысленно, а двигать ключевой источник за спину означает уже не
 *  "другой пресет", а другую схему. И тумблер "риг едет за камерой" сюда тоже не
 *  входит - это способ работы, а не внешний вид кадра. */
USTRUCT(BlueprintType)
struct FLightPreset
{
	GENERATED_BODY()

	/** Отображаемое имя - оно же уходит в HUD. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	FString Name;

	// --- Солнце уровня (ADirectionalLight) ---

	/** Светит ли солнце вообще. В студийных пресетах гасится: смешивать
	 *  направленный свет улицы с трёхточечной схемой значит получить четвёртый
	 *  источник, которого никто не ставил. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	bool bSunEnabled = true;

	/** Яркость солнца в люксах (UE5 считает свет в физических единицах). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float SunIntensity = 10.0f;

	/** Цветовая температура солнца, кельвины: 5500 - полдень, ниже - закат. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float SunTemperature = 5500.0f;

	/** Куда солнце смотрит: тангаж (отрицательный - сверху вниз) и рыскание.
	 *  Меняет заодно и НЕБО - у солнца в этом уровне включён Atmosphere Sun
	 *  Light, так что угол это ещё и время суток. Не побочный эффект, а
	 *  единственный способ получить закатный свет, не подкрашивая его вручную. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float SunPitch = -45.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float SunYaw = -30.0f;

	/** Множитель небесного (рассеянного) света. Гасить его в ноль не стоит даже
	 *  в студии: тени станут угольно-чёрными, и всё, что не попало под ключевой
	 *  источник, исчезнет из кадра вовсе. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float SkyIntensity = 1.0f;

	// --- Студийный риг (три направленных источника) ---

	/** Включён ли риг. Направленные, а не точечные, и это не мелочь: структура
	 *  в этом проекте бывает и в сотню клеток, и в семь миллионов, а у
	 *  направленного света нет ни расстояния, ни затухания - он одинаково верен
	 *  на любом размере. Точечный риг пришлось бы масштабировать под габарит. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	bool bStudioEnabled = false;

	/** Ключевой - главный источник, единственный, который отбрасывает тени.
	 *  Остальные два теней не дают намеренно: две лишние тени от заполняющего и
	 *  контрового читаются как грязь, а в UE каждая ещё и стоит отдельного
	 *  прохода. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float KeyIntensity = 8.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float KeyTemperature = 6000.0f;

	/** Заполняющий - подсвечивает то, что ключевой оставил в тени. Слабее
	 *  ключевого примерно втрое: сравняешь - получишь плоскую картинку без
	 *  объёма, ради которого схема и ставится. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float FillIntensity = 3.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float FillTemperature = 4500.0f;

	/** Контровой - светит сзади-сверху и обводит силуэт светлым кантом,
	 *  отделяя фигуру от фона. На пористой структуре с полостями он же
	 *  проявляет глубину: свет проходит насквозь там, где есть дыры. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float RimIntensity = 6.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Lighting")
	float RimTemperature = 7500.0f;
};

/** Таблица световых пресетов - константы кода (см. RulePresets/RenderPresets за
 *  той же формой). */
namespace LightPresets
{
	/** Все пресеты в порядке показа. Индекс - то, чем их применяют. */
	CELLULARAUTOMATA_API const TArray<FLightPreset>& GetAll();
}
