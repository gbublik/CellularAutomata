#pragma once

#include "CoreMinimal.h"
#include "Automata/Sonification/ESonificationShape.h"
#include "Automata/Sonification/SonificationFeatures.h"
#include "Automata/Sonification/SonificationParams.h"
#include "Orchestration/GenerationHistory.h"

/** Вся математика сонификации - свободные функции над чужим массивом замеров,
 *  без UObject, актора и звукового устройства.
 *
 *  Тот же идиом и та же причина, что у CellAging/ColorRamp/GenerationHistory:
 *  неочевидное здесь - устойчивость к неравномерному шагу по X, вырожденные
 *  окна и поведение на вымирании, и всё это покрывается автотестами headless,
 *  без PIE, рендера и аудиоустройства. Компонент-мост остаётся тонким:
 *  прочитать, сгладить, разослать.
 *
 *  Потокобезопасности нет и не требуется: единственный вызывающий - тик
 *  компонента, то есть игровой поток, как и все записи в саму историю. */
namespace SonificationCurve
{
	/** Индекс первого замера окна.
	 *
	 *  Окно задаётся в ПОКОЛЕНИЯХ (WindowGenerations), но с полом по числу
	 *  замеров (MinWindowSamples): при StepsPerRender 256 в шестьдесят четыре
	 *  поколения не попадёт ни одного замера, и окно обязано расшириться назад,
	 *  сколько бы поколений оно ни накрыло. В этом и состоит вся устойчивость к
	 *  дырам - окно определено по значению X, а не по количеству точек.
	 *
	 *  INDEX_NONE, если история пуста. */
	CELLULARAUTOMATA_API int32 FindWindowStart(const TArray<FGenerationSample>& History,
		int64 WindowGenerations, int32 MinSamples);

	/** МНК-наклон y = ln(1 + AliveCount) по x = Generation на отрезке
	 *  [First, Last] включительно.
	 *
	 *  false, если пригодных замеров меньше двух или знаменатель вырожден -
	 *  все замеры на одном поколении. Это не теоретический случай:
	 *  NoteRendered() правит последний замер НА МЕСТЕ при каждом рендере, а
	 *  рендер дёргается на каждое движение камеры при включённом срезе, так что
	 *  на паузе окно вполне может состоять из одного поколения.
	 *
	 *  OutCentroidX - фактическое среднее по X, а не середина отрезка: на нём
	 *  строится кривизна, и середина отрезка при дырах врала бы. */
	CELLULARAUTOMATA_API bool FitLogSlope(const TArray<FGenerationSample>& History,
		int32 First, int32 Last, double& OutSlope, double& OutCentroidX);

	/** Кривизна как разность наклонов двух половин окна, делённая на разность
	 *  их фактических центроидов.
	 *
	 *  Половины делятся по ЗНАЧЕНИЮ поколения (nMid = (nFirst + nLast)/2), а не
	 *  по середине массива: дыра сдвинула бы середину массива в сторону и
	 *  перекосила бы половины.
	 *
	 *  Квадратичный МНК (y ~ c0 + c1*x + c2*x^2) отвергнут не за сложность, а за
	 *  деградацию: на трёх точках он даёт точное, но бессмысленное решение -
	 *  парабола через три точки существует всегда. Половинная схема в том же
	 *  случае честно говорит "не знаю" и возвращает 0.0 - тот же контракт
	 *  "мерить не по чему", что у FitGrowthExponent().
	 *
	 *  Выдаёт сразу обе формы изгиба: сырую кривизну (изменение наклона на
	 *  поколение) и безразмерное отношение Bend в [-1, 1], которым и ведётся
	 *  звук - см. FSonificationFeatures::Bend.
	 *
	 *  false, если половин не набралось (нужно не меньше четырёх замеров) или
	 *  центроиды совпали. */
	CELLULARAUTOMATA_API bool FitBend(const TArray<FGenerationSample>& History,
		int32 First, int32 Last, double& OutCurvature, double& OutBend);

	/** Форма кривой по уже посчитанным признакам. Порядок проверок фиксирован,
	 *  первое совпадение выигрывает - см. реализацию.
	 *
	 *  Смотрит и на наклон, и на показатель роста: в лог-координатах линейный
	 *  рост неотличим от лёгкого насыщения, и различает их только d (см.
	 *  FSonificationParams::SaturationDimension). */
	CELLULARAUTOMATA_API ESonificationShape ClassifyShape(const FSonificationFeatures& Features,
		const FSonificationParams& Params);

	/** Полное измерение окна. Единственная функция, которую зовёт компонент.
	 *
	 *  Показатель роста берётся из GenerationHistory::FitGrowthExponent() по
	 *  ВСЕЙ истории, а не по окну: размерность структуры - медленная глобальная
	 *  характеристика, и мерить её по шестидесяти четырём поколениям значило бы
	 *  ловить шум. Своей математики для неё здесь нет ни строчки. */
	CELLULARAUTOMATA_API FSonificationFeatures ComputeFeatures(
		const TArray<FGenerationSample>& History, const FSonificationParams& Params);

	/** Экспоненциальное сглаживание к цели: alpha = 1 - exp(-Dt/Tau).
	 *
	 *  Именно так, а не FMath::Lerp(Current, Target, K): кадр здесь плавает от
	 *  восьми миллисекунд до секунды с лишним, и постоянная времени обязана
	 *  быть в секундах, а не в кадрах. У формы через exp есть полугрупповое
	 *  свойство - два шага по Dt/2 дают ровно то же, что один по Dt, - и это
	 *  единственное, что делает звук независимым от частоты кадров. У Lerp его
	 *  нет, там результат зависит от того, как нарезали время.
	 *
	 *  Tau <= 0 - без сглаживания, вернётся сама цель. */
	CELLULARAUTOMATA_API float SmoothTowards(float Current, float Target,
		float TimeConstantSeconds, float DeltaSeconds);
}