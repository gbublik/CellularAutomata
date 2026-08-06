#pragma once

#include "CoreMinimal.h"
#include "Automata/Persistence/AutomatonSaveHeader.h"

class FCellGrid;

/** Сериализация состояния автомата в байты и обратно - гибридный контейнер
 *  .casave: JSON-шапка (FAutomatonSaveHeader, эволюционирует через reflection)
 *  + компактная бинарная полезная нагрузка клеток (при миллионах клеток JSON
 *  непригоден) + PNG-миниатюра. Plain namespace, не UObject - та же идиома,
 *  что CellAging/CellSelection/CellMeshBuilder. Работает ТОЛЬКО с байтами в
 *  памяти: файловый ввод-вывод, диалоги, снимок скриншота и применение
 *  UPROPERTY - на AAutomataOrchestrator::SaveState()/SaveStateAs()/
 *  LoadStateFromFile()/WriteStateToFile()/CaptureThumbnailPng().
 *
 *  Формат контейнера (v3, всё little-endian):
 *    uint32  Magic = "CASV"
 *    uint32  ContainerVersion            // версия КОМПОНОВКИ байт контейнера
 *    int32   JsonUtf8Length
 *    uint8[] JSON-шапка в UTF-8          // без терминатора
 *    int32   CellCount                   // авторитетный счётчик для цикла чтения
 *    CellCount x { int32 X, int32 Y, int32 Z, uint8 Age }
 *    int32   InitialCellCount            // раздел "изначальное состояние" - точка возврата R
 *    InitialCellCount x { int32 X, int32 Y, int32 Z }   // без возраста - R всегда сбрасывает в 0
 *    int32   ThumbnailPngLength          // раздел PNG-миниатюры - 0 значит "миниатюры нет"
 *    uint8[ThumbnailPngLength]           // сырые байты PNG
 *
 *  Раздел InitialCells - это AAutomataOrchestrator::InitialStateCells
 *  (см. её doc-comment): без него загрузка файла подменяла бы точку возврата
 *  R текущим (возможно уже проэволюционировавшим) снимком клеток, и R после
 *  загрузки возвращал бы не туда, куда возвращал бы до сохранения. Раздел
 *  может быть пустым (0 клеток) - это ЗНАЧИМОЕ состояние "в этой сессии
 *  ещё не было StartFromSelection()/загрузки", не ошибка. AAutomataOrchestrator::
 *  WriteStateToFile() пишет в CellCount-секцию тот же InitialStateCells, что
 *  и в InitialCellCount-секцию (сохраняется именно изначальный паттерн, не
 *  текущее живое состояние Grid - см. её doc-comment) - обе секции на
 *  практике совпадают, формат при этом не меняется: обе остаются раздельными
 *  полями ради чтения старых файлов, где они хранили разные вещи (текущий
 *  живой снимок vs. точка возврата).
 *
 *  Раздел миниатюры (ThumbnailPngLength/PNG-байты) - AAutomataOrchestrator::
 *  CaptureThumbnailPng()'s результат, снимок ТЕКУЩЕГО вида (текущая камера,
 *  текущая живая симуляция) в момент сохранения - НЕ вида на сохраняемый
 *  изначальный паттерн; ThumbnailPngLength == 0 - тоже ЗНАЧИМОЕ состояние
 *  (захват не удался, или файл создан до появления этого раздела), не
 *  ошибка.
 *
 *  JSON пишется явным length-prefixed UTF-8, а НЕ через FString << Ar:
 *  архивная кодировка FString (UTF-16/ANSI по знаку SaveNum) - внутренняя
 *  деталь движка, а шапка должна читаться глазами в hex/текстовом редакторе.
 *
 *  Две версии - две роли: ContainerVersion бампается только при изменении
 *  компоновки байт (1 -> 2: добавление раздела InitialCells; 2 -> 3:
 *  добавление раздела миниатюры); FormatVersion (внутри JSON) - при
 *  изменении семантики шапки или формата записи клетки. Добавление
 *  JSON-поля (например, ThumbnailPngLength) не трогает ни одну версию.
 *  Обратная совместимость: файлы v1 (без раздела InitialCells) и v2 (без
 *  миниатюры) по-прежнему читаются - ReadSave() для v1-файлов сама выводит
 *  OutInitialCells из OutCells (старое поведение AAutomataOrchestrator::
 *  LoadStateFromFile() до появления этого раздела); для файлов < v3
 *  OutThumbnailPng остаётся пустым. Вызывающей стороне не нужно знать о
 *  версиях. Сжатия полезной нагрузки клеток нет (future work: bCompressed +
 *  FCompression). */
namespace AutomatonStateSerializer
{
	inline constexpr uint32 SaveMagic = 0x56534143; // на диске: 43 41 53 56 = "CASV"
	inline constexpr uint32 ContainerVersion = 3;

	/** Одна клетка полезной нагрузки: координата + возраст (см. FCellGrid::GetAge()). */
	struct FSavedCell
	{
		FIntVector Cell = FIntVector::ZeroValue;
		uint8 Age = 0;
	};

	/** Заливает клетки в (обычно свежесозданную) сетку: SetAlive() + SetAge(). */
	CELLULARAUTOMATA_API void ApplyCells(const TArray<FSavedCell>& Cells, FCellGrid& Grid);

	/** Сдвиг, переносящий центр AABB набора клеток в начало координат - то, что
	 *  AAutomataOrchestrator::WriteStateToFile() применяет к паттерну ПЕРЕД
	 *  записью в файл.
	 *
	 *  Зачем. Извлечённое выделение (Enter) сохраняет мировые координаты того
	 *  места, где рой вырос, - а это могут быть тысячи клеток от нуля. Правила
	 *  автомата трансляционно инвариантны, так что для симуляции координаты
	 *  ничего не значат, но камера при загрузке файла стоит там, где стояла, и
	 *  паттерн из такого файла оказывается далеко за кадром. Файл - документ,
	 *  открываемый в произвольной сессии с произвольной камерой, поэтому узор в
	 *  нём лежит вокруг нуля; живой Grid и InitialStateCells при этом не
	 *  трогаются (сохранение по-прежнему ничего не мутирует).
	 *
	 *  Все три компоненты сдвига ЧЁТНЫЕ, и это не косметика: ГЦК/ОЦК-паттерны
	 *  живут на подрешётке, заданной чётностью координат (см. ECellParityFilter),
	 *  и сдвиг на нечётный вектор перенёс бы узор на соседнюю подрешётку -
	 *  первое же пересевание после загрузки (N/Y) ушло бы мимо него. Ровно та же
	 *  причина, по которой ParityFilter вообще попадает в шапку. Цена - центр
	 *  промахивается мимо нуля не больше чем на клетку.
	 *
	 *  Центр берётся по AABB (по (Min+Max)/2), а не по среднему: центроид
	 *  утянуло бы в плотную часть узора, а в кадр вписывается именно габарит.
	 *  Пустой набор даёт нулевой сдвиг. */
	CELLULARAUTOMATA_API FIntVector ComputeCenteringOffset(const TArray<FIntVector>& Cells);

	/** Собирает контейнер в OutBytes. CellCount/InitialCellCount/
	 *  ThumbnailPngLength в шапке выставляются здесь из Cells.Num()/
	 *  InitialCells.Num()/ThumbnailPng.Num() - вызывающий не может создать
	 *  расхождение, которое потом отверг бы ReadSave(). InitialCells - это
	 *  AAutomataOrchestrator::InitialStateCells (точка возврата R),
	 *  ThumbnailPng - результат CaptureThumbnailPng(); оба могут быть
	 *  пустыми. false только при ошибке сериализации JSON. */
	CELLULARAUTOMATA_API bool WriteSave(const FAutomatonSaveHeader& Header, const TArray<FSavedCell>& Cells,
		const TArray<FIntVector>& InitialCells, const TArray64<uint8>& ThumbnailPng, TArray64<uint8>& OutBytes);

	/** Разбирает контейнер. Любой отказ (не тот magic, версия новее, обрезанный
	 *  файл, битый JSON, расхождение счётчиков) - Warning-лог с причиной и
	 *  false; выходные параметры при этом не имеют осмысленного содержимого.
	 *  Для файлов v1 (нет раздела InitialCells) OutInitialCells выводится из
	 *  OutCells - см. doc-comment namespace'а. Для файлов < v3 (нет раздела
	 *  миниатюры) OutThumbnailPng остаётся пустым - валидно, не ошибка. */
	CELLULARAUTOMATA_API bool ReadSave(const TArray64<uint8>& Bytes, FAutomatonSaveHeader& OutHeader,
		TArray<FSavedCell>& OutCells, TArray<FIntVector>& OutInitialCells, TArray64<uint8>& OutThumbnailPng);
}
