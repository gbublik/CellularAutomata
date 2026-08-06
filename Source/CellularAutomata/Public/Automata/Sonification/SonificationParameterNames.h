#pragma once

#include "CoreMinimal.h"

/** Имена входов графа MetaSound - единственное место в проекте, где они
 *  записаны.
 *
 *  Разделение обязанностей здесь ровно то же, что у HUD: C++ поставляет данные
 *  и события, а вёрстка - то есть сам граф синтеза, тембры и огибающие - живёт
 *  в ассете и пишется не отсюда. Мост обязан оставаться тонким: прочитать,
 *  сгладить, разослать.
 *
 *  Имена латиницей и PascalCase - как их видно во входах графа. Чтобы список
 *  не разошёлся с тем, что реально шлётся, есть
 *  AAutomataOrchestrator::LogSonificationContract() (кнопка в панели): она
 *  печатает весь контракт с расшифровкой по-русски прямо из этих констант.
 *  Инструкция, которая физически не может устареть.
 *
 *  ВАЖНОЕ ПРО ТРИГГЕРЫ: SetTriggerParameter() у движка молча не делает ничего,
 *  если компонент не играет (весь метод под if(IsPlaying())). Поэтому фоновый
 *  UAudioComponent обязан играть НЕПРЕРЫВНО с BeginPlay, а не запускаться на
 *  событие - иначе вымирание и ресид терялись бы беззвучно и неотлаживаемо.
 *  С float-параметрами наоборот: они мержатся в InstanceParameters и
 *  применяются при следующем Play(), на чём и построен клик по клетке. */
namespace SonificationParameters
{
	// ------------------------------------------------------- фон, аналоговое

	/** 0..1. Население в логарифме: ln(1+N)/ln(1+FullScalePopulation). */
	extern CELLULARAUTOMATA_API const FName Population;

	/** -1..1. Знаковая относительная скорость роста. Плюс - растёт. */
	extern CELLULARAUTOMATA_API const FName Slope;

	/** 0..1. Положительная часть Slope - отдельным фейдером, потому что задача
	 *  и звучала как "рост одним звуком, падение другим", а это два разных
	 *  слоя в графе, а не один со знаком. */
	extern CELLULARAUTOMATA_API const FName Growth;

	/** 0..1. Отрицательная часть Slope. */
	extern CELLULARAUTOMATA_API const FName Decay;

	/** -1..1. Изгиб: плюс - разгон, минус - выдыхается, -1 - разворот. */
	extern CELLULARAUTOMATA_API const FName Curvature;

	/** 0..1. Плотность шуршания: длина пути кривой на поколение. */
	extern CELLULARAUTOMATA_API const FName Activity;

	/** 0..1. Ходит и никуда не приходит - осциллятор либо кипящее равновесие. */
	extern CELLULARAUTOMATA_API const FName Oscillation;

	/** 0..1. Размерность структуры d/3: нить, оболочка, объём. Меняется
	 *  медленно - это тембр, а не громкость. */
	extern CELLULARAUTOMATA_API const FName Dimension;

	/** Тот же показатель без нормировки - на случай, если графу удобнее сырое. */
	extern CELLULARAUTOMATA_API const FName DimensionRaw;

	/** 0..1. Фактические поколения в секунду - темп. */
	extern CELLULARAUTOMATA_API const FName Rate;

	/** 0..1. Симуляция сейчас работает. Гейт против дрона на паузе; НЕ падает,
	 *  пока идёт фоновый счёт: диспатч на семь секунд - это работа, а не
	 *  простой, и приглушать фон ровно тогда, когда машина считает тяжелее
	 *  всего, было бы враньём. */
	extern CELLULARAUTOMATA_API const FName Liveness;

	/** 0..1. Насколько окну можно верить. При больших StepsPerRender колебания
	 *  внутри пропущенных поколений не записаны нигде, и осцилляционный слой
	 *  надо приглушать, а не выдавать шум за музыку. */
	extern CELLULARAUTOMATA_API const FName Confidence;

	/** Порядковый номер ESonificationShape - если графу захочется переключать
	 *  слой целиком. Звук вести этим НЕ следует: на границе порога он щёлкал бы
	 *  туда-сюда, для того и даны непрерывные величины выше. */
	extern CELLULARAUTOMATA_API const FName Shape;

	// ----------------------------------------------------------- фон, флаги

	/** Прогон идёт (Play либо удержанный быстрый шаг). */
	extern CELLULARAUTOMATA_API const FName Running;

	/** Сетка пуста. */
	extern CELLULARAUTOMATA_API const FName Extinct;

	// -------------------------------------------------------- фон, триггеры

	/** Сетка только что опустела. Ловится фронтом по числу живых, поэтому
	 *  срабатывает и когда сетку опустошили руками на паузе (Delete), а не
	 *  только когда её доконал шаг. */
	extern CELLULARAUTOMATA_API const FName OnExtinction;

	/** Автоперекат сида после вымирания (Shift+N). */
	extern CELLULARAUTOMATA_API const FName OnReseed;

	/** Прогон пошёл. */
	extern CELLULARAUTOMATA_API const FName OnStart;

	/** Прогон остановлен. */
	extern CELLULARAUTOMATA_API const FName OnStop;

	/** Счётчик поколений начат заново - любым из путей "начать сначала". */
	extern CELLULARAUTOMATA_API const FName OnReset;

	/** Поколения СОСТОЯЛИСЬ. Именно диспатч, а не поколение: один заход при
	 *  батчинге считает сразу пачку до StepsPerRender, и промежуточных
	 *  поколений на игровом потоке не существует вовсе. Щелчок на каждое
	 *  поколение здесь невозможен в принципе, не по лени, а по устройству. */
	extern CELLULARAUTOMATA_API const FName OnDispatch;

	/** Сколько поколений принёс последний заход - чтобы граф мог отличить
	 *  одиночный шаг от пачки в двести пятьдесят шесть. */
	extern CELLULARAUTOMATA_API const FName DispatchGenerations;

	// ---------------------------------------------------- клик по клетке

	/** 0..1. Возраст клетки, поделённый на AgeColorMaxAge - ТЕМ ЖЕ числом,
	 *  которым клетка красится. Побочный эффект приятный и не случайный:
	 *  высота ноты совпадает с цветом, красная клетка звучит выше синей. */
	extern CELLULARAUTOMATA_API const FName Age01;

	/** Возраст как есть, в поколениях. */
	extern CELLULARAUTOMATA_API const FName AgeRaw;

	/** 0..1, уже готовая высота - чтобы граф не занимался арифметикой. */
	extern CELLULARAUTOMATA_API const FName Pitch01;
}