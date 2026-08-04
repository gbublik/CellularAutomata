#pragma once

#include "CoreMinimal.h"
#include "Automata/Simulation/Neighborhood.h"
#include "Automata/Simulation/LatticeNeighborhood.h"
#include "Automata/Generation/CellParityFilter.h"
#include "AutomatonSaveHeader.generated.h"

/** JSON-шапка файла сохранения (.casave): правила автомата + геометрия сетки +
 *  параметры генерации. Сериализуется через FJsonObjectConverter, поэтому
 *  добавление нового параметра - это ОДНО UPROPERTY-поле здесь (плюс по строке
 *  копирования в BuildSaveHeader()/ApplySaveHeader() оркестратора): отсутствующее
 *  в старом файле поле остаётся дефолтом структуры, неизвестное поле в новом
 *  файле молча игнорируется - миграций не нужно.
 *
 *  FormatVersion версионирует СЕМАНТИКУ шапки и формат бинарной записи клетки;
 *  бампается только при несовместимом изменении (например, сжатие полезной
 *  нагрузки), НЕ при добавлении полей. Компоновку байт самого контейнера
 *  версионирует отдельный ContainerVersion (см. AutomatonStateSerializer.h).
 *
 *  Настройки рендера/камеры (Speed, StepsPerRender, материалы, меши) сюда
 *  сознательно НЕ входят - это настройки просмотра, не состояние автомата. */
USTRUCT()
struct FAutomatonSaveHeader
{
	GENERATED_BODY()

	UPROPERTY()
	int32 FormatVersion = 1;

	// --- Правила ---

	UPROPERTY()
	TArray<int32> BirthCounts;

	UPROPERTY()
	TArray<int32> SurvivalCounts;

	/** В JSON пишется строкой ("Moore"/"VonNeumann") - читаемо и стабильно
	 *  к перестановке значений enum'а. */
	UPROPERTY()
	ENeighborhood Neighborhood = ENeighborhood::Moore;

	/** УСТАРЕВШЕЕ, только для чтения старых файлов. Недолго существовал
	 *  отдельный радиус соседства, и нынешний ENeighborhood::VonNeumann2
	 *  записывался как VonNeumann с радиусом 2. ApplySaveHeader() отображает
	 *  эту пару обратно в VonNeumann2; без миграции такой файл загрузился бы
	 *  как обычный VonNeumann - молча, с 6 соседями вместо 24.
	 *
	 *  Новые сохранения поле не пишут (остаётся дефолт), и удалить его нельзя,
	 *  пока могут встретиться файлы того периода. */
	UPROPERTY()
	int32 NeighborhoodRadius = 1;

	/** Общее число состояний клетки (см. AAutomataOrchestrator::States) -
	 *  2 (дефолт) значит классический бинарный автомат. Аддитивное поле -
	 *  версия контейнера/формата не бампается (см. doc-comment класса):
	 *  файл версии до появления States просто восстановится с дефолтом 2.
	 *  Промежуточное decay-состояние клетки НЕ сохраняется - как и Age
	 *  сегодня, загруженный/извлечённый паттерн всегда стартует свежим
	 *  (state 1, полностью живая), см. AutomatonStateSerializer - раздел
	 *  InitialCells хранит только координаты, без возраста/состояния. */
	UPROPERTY()
	int32 States = 2;

	// --- Геометрия сетки ---

	UPROPERTY()
	float CellSize = 100.0f;

	/** Растяжение решётки по Z (см. AAutomataOrchestrator::LatticeZScale).
	 *  Аддитивное поле - ни версия контейнера, ни версия формата не бампаются
	 *  (тот же довод, что у States выше): файл, записанный до его появления,
	 *  восстановится с дефолтом 1.0, а это в точности его тогдашняя
	 *  семантика - решётка с равным шагом по осям. */
	UPROPERTY()
	float LatticeZScale = 1.0f;

	/** Какие узлы решётки заселены (см. FStateGeneratorParams::ParityFilter).
	 *  Тоже аддитивное поле, и оно закрывает существовавшую дыру: сами клетки
	 *  сохранялись всегда, поэтому подрешётка переживала загрузку, а вот
	 *  первое же нажатие N/Y после загрузки пересевало по ФИЛЬТРУ ИЗ ПАНЕЛИ -
	 *  то есть, как правило, на простой кубической решётке вместо ГЦК/ОЦК, и
	 *  структура молча теряла форму. */
	UPROPERTY()
	ECellParityFilter ParityFilter = ECellParityFilter::None;

	/** Соседство, заданное списком смещений вместо оболочки (см.
	 *  ELatticeNeighborhood). Аддитивное поле; дефолт Shells означает "берём
	 *  Neighborhood выше", то есть в точности поведение файлов, записанных до
	 *  его появления. */
	UPROPERTY()
	ELatticeNeighborhood NeighborhoodShape = ELatticeNeighborhood::Shells;

	UPROPERTY()
	int32 ChunkSize = 16;

	UPROPERTY()
	FIntVector GridSize = FIntVector(100, 100, 100);

	// --- Параметры генерации ---

	UPROPERTY()
	int32 Seed = 0;

	UPROPERTY()
	int32 Amount = 1000;

	UPROPERTY()
	int32 SpawnRadius = 10;

	UPROPERTY()
	float ClusterFactor = 0.7f;

	/** Число клеток в бинарной части - должно совпасть со счётчиком перед
	 *  полезной нагрузкой (sanity-проверка от порчи файла). WriteSave()
	 *  выставляет его сам, вручную заполнять не нужно. */
	UPROPERTY()
	int32 CellCount = 0;

	/** Число клеток в разделе "изначальное состояние" (см.
	 *  AutomatonStateSerializer - InitialCells, тот же sanity-паттерн, что и
	 *  CellCount, для отдельного раздела с точкой возврата R). Появилось в
	 *  ContainerVersion 2 - в файлах версии 1 раздела не было, WriteSave()
	 *  выставляет поле сам. */
	UPROPERTY()
	int32 InitialCellCount = 0;

	/** Размер PNG-миниатюры (в байтах) в разделе миниатюры контейнера - тот
	 *  же sanity-паттерн, что CellCount/InitialCellCount. Появилось в
	 *  ContainerVersion 3 - в файлах версий 1/2 раздела не было, 0 означает
	 *  "миниатюры нет" (промах захвата или старый файл), не ошибку.
	 *  WriteSave() выставляет поле сам. */
	UPROPERTY()
	int32 ThumbnailPngLength = 0;
};
