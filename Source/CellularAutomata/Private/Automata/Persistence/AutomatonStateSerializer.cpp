#include "Automata/Persistence/AutomatonStateSerializer.h"
#include "Automata/Grid/CellGrid.h"
#include "JsonObjectConverter.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"

namespace
{
	// int32 X + int32 Y + int32 Z + uint8 Age
	constexpr int64 BytesPerCell = 13;

	// int32 X + int32 Y + int32 Z - без возраста, InitialCells всегда
	// сбрасывается в возраст 0 при ResetToInitialState().
	constexpr int64 BytesPerInitialCell = 12;

	// Magic + ContainerVersion + JsonUtf8Length
	constexpr int64 FixedPrefixBytes = 12;

	/** Середина отрезка [MinValue, MaxValue], опущенная до ближайшего чётного
	 *  снизу. Считается в int64: сумма двух int32-координат переполняет int32,
	 *  а координаты клеток ничем не ограничены. Деление - именно
	 *  DivideAndRoundDown (настоящий floor), а не '/': у отрицательных координат
	 *  усечение к нулю дало бы другой ответ, и это ровно та же ловушка, что в
	 *  чанковой арифметике FDenseCellGrid. */
	int32 EvenMidpoint(int32 MinValue, int32 MaxValue)
	{
		const int64 Sum = static_cast<int64>(MinValue) + static_cast<int64>(MaxValue);
		const int64 Center = FMath::DivideAndRoundDown<int64>(Sum, 2);
		return static_cast<int32>(FMath::DivideAndRoundDown<int64>(Center, 2) * 2);
	}
}

void AutomatonStateSerializer::ApplyCells(const TArray<FSavedCell>& Cells, FCellGrid& Grid)
{
	for (const FSavedCell& Saved : Cells)
	{
		Grid.SetAlive(Saved.Cell, true);
		Grid.SetAge(Saved.Cell, Saved.Age);
	}
}

FIntVector AutomatonStateSerializer::ComputeCenteringOffset(const TArray<FIntVector>& Cells)
{
	if (Cells.Num() == 0)
	{
		return FIntVector::ZeroValue;
	}

	FIntVector Min = Cells[0];
	FIntVector Max = Cells[0];
	for (const FIntVector& Cell : Cells)
	{
		Min.X = FMath::Min(Min.X, Cell.X);
		Min.Y = FMath::Min(Min.Y, Cell.Y);
		Min.Z = FMath::Min(Min.Z, Cell.Z);
		Max.X = FMath::Max(Max.X, Cell.X);
		Max.Y = FMath::Max(Max.Y, Cell.Y);
		Max.Z = FMath::Max(Max.Z, Cell.Z);
	}

	return FIntVector(
		-EvenMidpoint(Min.X, Max.X),
		-EvenMidpoint(Min.Y, Max.Y),
		-EvenMidpoint(Min.Z, Max.Z));
}

bool AutomatonStateSerializer::WriteSave(const FAutomatonSaveHeader& Header, const TArray<FSavedCell>& Cells,
	const TArray<FIntVector>& InitialCells, const TArray64<uint8>& ThumbnailPng, TArray64<uint8>& OutBytes)
{
	// CellCount/InitialCellCount/ThumbnailPngLength выставляем сами из
	// фактических наборов - вызывающий не может создать расхождение, которое
	// потом отверг бы ReadSave().
	FAutomatonSaveHeader HeaderCopy = Header;
	HeaderCopy.CellCount = Cells.Num();
	HeaderCopy.InitialCellCount = InitialCells.Num();
	HeaderCopy.ThumbnailPngLength = ThumbnailPng.Num();

	FString JsonString;
	if (!FJsonObjectConverter::UStructToJsonObjectString(HeaderCopy, JsonString))
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::WriteSave: не удалось сериализовать шапку в JSON"));
		return false;
	}

	const FTCHARToUTF8 JsonUtf8(*JsonString);
	int32 JsonUtf8Length = JsonUtf8.Length();

	OutBytes.Reset();
	OutBytes.Reserve(FixedPrefixBytes + JsonUtf8Length + sizeof(int32) + BytesPerCell * Cells.Num()
		+ sizeof(int32) + BytesPerInitialCell * InitialCells.Num()
		+ sizeof(int32) + ThumbnailPng.Num());

	FMemoryWriter64 Writer(OutBytes);

	uint32 Magic = SaveMagic;
	uint32 Version = ContainerVersion;
	Writer << Magic;
	Writer << Version;
	Writer << JsonUtf8Length;
	Writer.Serialize(const_cast<ANSICHAR*>(JsonUtf8.Get()), JsonUtf8Length);

	int32 CellCount = Cells.Num();
	Writer << CellCount;
	for (const FSavedCell& Saved : Cells)
	{
		FIntVector Cell = Saved.Cell;
		uint8 Age = Saved.Age;
		Writer << Cell.X;
		Writer << Cell.Y;
		Writer << Cell.Z;
		Writer << Age;
	}

	int32 InitialCellCount = InitialCells.Num();
	Writer << InitialCellCount;
	for (const FIntVector& InitialCell : InitialCells)
	{
		FIntVector Cell = InitialCell;
		Writer << Cell.X;
		Writer << Cell.Y;
		Writer << Cell.Z;
	}

	int32 ThumbnailPngLength = ThumbnailPng.Num();
	Writer << ThumbnailPngLength;
	if (ThumbnailPngLength > 0)
	{
		Writer.Serialize(const_cast<uint8*>(ThumbnailPng.GetData()), ThumbnailPngLength);
	}

	return true;
}

bool AutomatonStateSerializer::ReadSave(const TArray64<uint8>& Bytes, FAutomatonSaveHeader& OutHeader,
	TArray<FSavedCell>& OutCells, TArray<FIntVector>& OutInitialCells, TArray64<uint8>& OutThumbnailPng)
{
	if (Bytes.Num() < FixedPrefixBytes)
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл обрезан (%lld байт - меньше минимального заголовка)"), Bytes.Num());
		return false;
	}

	// FMemoryReader64 в движке нет (в отличие от FMemoryWriter64) -
	// 64-битный вариант чтения это FMemoryReaderView поверх FMemoryView.
	FMemoryReaderView Reader(MakeMemoryView(Bytes));

	uint32 Magic = 0;
	uint32 Version = 0;
	int32 JsonUtf8Length = 0;
	Reader << Magic;
	Reader << Version;
	Reader << JsonUtf8Length;

	if (Magic != SaveMagic)
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: это не файл сохранения автомата (magic 0x%08X вместо 0x%08X)"), Magic, SaveMagic);
		return false;
	}
	if (Version > ContainerVersion)
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл создан более новой версией (контейнер v%u, поддерживается до v%u)"), Version, ContainerVersion);
		return false;
	}
	if (JsonUtf8Length < 2 || FixedPrefixBytes + static_cast<int64>(JsonUtf8Length) + static_cast<int64>(sizeof(int32)) > Bytes.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл обрезан (JSON-шапка %d байт не помещается в файл из %lld байт)"), JsonUtf8Length, Bytes.Num());
		return false;
	}

	TArray<uint8> JsonUtf8;
	JsonUtf8.SetNumUninitialized(JsonUtf8Length);
	Reader.Serialize(JsonUtf8.GetData(), JsonUtf8Length);

	const FUTF8ToTCHAR JsonTchar(reinterpret_cast<const ANSICHAR*>(JsonUtf8.GetData()), JsonUtf8Length);
	const FString JsonString(JsonTchar.Length(), JsonTchar.Get());

	OutHeader = FAutomatonSaveHeader();
	if (!FJsonObjectConverter::JsonObjectStringToUStruct(JsonString, &OutHeader))
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: не удалось разобрать JSON-шапку"));
		return false;
	}
	if (OutHeader.FormatVersion > 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл создан более новой версией (формат v%d, поддерживается до v1)"), OutHeader.FormatVersion);
		return false;
	}

	int32 CellCount = 0;
	Reader << CellCount;
	if (CellCount < 0 || CellCount != OutHeader.CellCount)
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл повреждён (счётчик клеток %d не совпадает с шапкой: %d)"), CellCount, OutHeader.CellCount);
		return false;
	}

	// v1 не знает раздела InitialCells - после основной полезной нагрузки
	// файл должен заканчиваться РОВНО там (точное совпадение); v2 добавляет
	// после неё ещё int32 InitialCellCount + сами клетки, поэтому здесь
	// достаточно места хотя бы под сам счётчик - точную длину раздела
	// проверим отдельно ниже, уже зная InitialCellCount.
	const int64 RemainingAfterCellCountField = Bytes.Num() - Reader.Tell();
	const int64 MainPayloadBytes = BytesPerCell * CellCount;
	const bool bHasInitialCellsSection = (Version >= 2);
	const bool bHasThumbnailSection = (Version >= 3);
	if (bHasInitialCellsSection
		? (RemainingAfterCellCountField < MainPayloadBytes + static_cast<int64>(sizeof(int32)))
		: (RemainingAfterCellCountField != MainPayloadBytes))
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл обрезан (полезная нагрузка %lld байт меньше ожидаемых %lld для %d клеток)"), RemainingAfterCellCountField, MainPayloadBytes, CellCount);
		return false;
	}

	OutCells.Reset();
	OutCells.SetNumUninitialized(CellCount);
	for (int32 Index = 0; Index < CellCount; ++Index)
	{
		FSavedCell& Saved = OutCells[Index];
		Reader << Saved.Cell.X;
		Reader << Saved.Cell.Y;
		Reader << Saved.Cell.Z;
		Reader << Saved.Age;
	}

	OutInitialCells.Reset();
	if (bHasInitialCellsSection)
	{
		int32 InitialCellCount = 0;
		Reader << InitialCellCount;
		if (InitialCellCount < 0 || InitialCellCount != OutHeader.InitialCellCount)
		{
			UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл повреждён (счётчик изначального состояния %d не совпадает с шапкой: %d)"), InitialCellCount, OutHeader.InitialCellCount);
			return false;
		}

		// v2 не знает раздела миниатюры - здесь файл должен заканчиваться
		// РОВНО после InitialCells (точное совпадение); v3 добавляет после
		// неё ещё int32 ThumbnailPngLength + сами байты PNG, поэтому здесь
		// достаточно места хотя бы под сам счётчик.
		const int64 InitialPayloadBytesAvailable = Bytes.Num() - Reader.Tell();
		const int64 InitialPayloadBytesExpected = BytesPerInitialCell * InitialCellCount;
		const bool bInitialSectionSizeOk = bHasThumbnailSection
			? (InitialPayloadBytesAvailable >= InitialPayloadBytesExpected + static_cast<int64>(sizeof(int32)))
			: (InitialPayloadBytesAvailable == InitialPayloadBytesExpected);
		if (!bInitialSectionSizeOk)
		{
			UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл обрезан (раздел изначального состояния %lld байт меньше ожидаемых %lld для %d клеток)"), InitialPayloadBytesAvailable, InitialPayloadBytesExpected, InitialCellCount);
			return false;
		}

		OutInitialCells.SetNumUninitialized(InitialCellCount);
		for (int32 Index = 0; Index < InitialCellCount; ++Index)
		{
			Reader << OutInitialCells[Index].X;
			Reader << OutInitialCells[Index].Y;
			Reader << OutInitialCells[Index].Z;
		}
	}
	else
	{
		// Файл v1 не хранил точку возврата R отдельно - воспроизводим
		// старое поведение AAutomataOrchestrator::LoadStateFromFile()
		// (точка возврата = загруженный снимок целиком), чтобы вызывающая
		// сторона могла оставаться версионно-агностичной.
		OutInitialCells.Reserve(OutCells.Num());
		for (const FSavedCell& Saved : OutCells)
		{
			OutInitialCells.Add(Saved.Cell);
		}
	}

	OutThumbnailPng.Reset();
	if (bHasThumbnailSection)
	{
		int32 ThumbnailPngLength = 0;
		Reader << ThumbnailPngLength;
		if (ThumbnailPngLength < 0 || ThumbnailPngLength != OutHeader.ThumbnailPngLength)
		{
			UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл повреждён (размер миниатюры %d не совпадает с шапкой: %d)"), ThumbnailPngLength, OutHeader.ThumbnailPngLength);
			return false;
		}

		const int64 RemainingForThumbnail = Bytes.Num() - Reader.Tell();
		if (RemainingForThumbnail != ThumbnailPngLength)
		{
			UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: файл обрезан (миниатюра %lld байт вместо ожидаемых %d)"), RemainingForThumbnail, ThumbnailPngLength);
			return false;
		}

		if (ThumbnailPngLength > 0)
		{
			OutThumbnailPng.SetNumUninitialized(ThumbnailPngLength);
			Reader.Serialize(OutThumbnailPng.GetData(), ThumbnailPngLength);
		}
	}
	// else: файл < v3 - раздела миниатюры не было, OutThumbnailPng остаётся
	// пустым (валидно, не ошибка).

	if (Reader.IsError())
	{
		UE_LOG(LogTemp, Warning, TEXT("AutomatonStateSerializer::ReadSave: ошибка чтения полезной нагрузки"));
		return false;
	}

	return true;
}
