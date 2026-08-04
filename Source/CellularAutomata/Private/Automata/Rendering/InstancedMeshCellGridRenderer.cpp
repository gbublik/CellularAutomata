#include "Automata/Rendering/InstancedMeshCellGridRenderer.h"

#include "Automata/Grid/CellGrid.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Algo/Sort.h"
#include "Algo/Reverse.h"
#include "Algo/RandomShuffle.h"

FInstancedMeshCellGridRenderer::FInstancedMeshCellGridRenderer(UInstancedStaticMeshComponent* InComponent)
	: Component(InComponent)
{
}

void FInstancedMeshCellGridRenderer::SetMesh(UStaticMesh* InMesh)
{
	Mesh = InMesh;
}

void FInstancedMeshCellGridRenderer::SetMaterial(UMaterialInterface* InMaterial)
{
	Material = InMaterial;
}

void FInstancedMeshCellGridRenderer::SetScaleMultiplier(float InScaleMultiplier)
{
	ScaleMultiplier = InScaleMultiplier;
}

void FInstancedMeshCellGridRenderer::Render(const FCellGrid& Grid, TArray<FCellRenderInstance>&& Instances)
{
	// Order/CameraLocation не влияют на однократный Render() - весь массив
	// всё равно уходит одним AddInstances() внутри одного и того же кадра,
	// порядок элементов внутри него не наблюдаем.
	BeginRender(Grid, MoveTemp(Instances), EChunkedRenderOrder::Sequential, FVector::ZeroVector);
	// Без ограничения на размер чанка - весь PendingInstances уходит одним
	// вызовом AddInstances(), как и раньше до появления чанкинга; цикл
	// формален (тела достаточно ровно одной итерации), но так BeginRender()/
	// AdvanceRenderChunk() остаются единственным местом с этой логикой.
	while (AdvanceRenderChunk(TNumericLimits<int32>::Max()))
	{
	}
}

void FInstancedMeshCellGridRenderer::BeginRender(const FCellGrid& Grid, TArray<FCellRenderInstance>&& Instances, EChunkedRenderOrder Order, const FVector& CameraLocation)
{
	PendingInstances = MoveTemp(Instances);
	PendingCursor = 0;
	LastTimings = FRenderTimings();

	UInstancedStaticMeshComponent* Comp = Component.Get();
	if (!Comp)
	{
		PendingInstances.Reset();
		UE_LOG(LogTemp, Warning, TEXT("FInstancedMeshCellGridRenderer: component is invalid"));
		return;
	}

	const double SetMeshStartSeconds = FPlatformTime::Seconds();
	if (Mesh.IsValid())
	{
		Comp->SetStaticMesh(Mesh.Get());
	}
	if (Material.IsValid())
	{
		Comp->SetMaterial(0, Material.Get());
	}
	LastTimings.SetMeshSeconds = FPlatformTime::Seconds() - SetMeshStartSeconds;

	const double ClearStartSeconds = FPlatformTime::Seconds();
	Comp->ClearInstances();
	// Строго ПОСЛЕ ClearInstances(): SetNumCustomDataFloats() пересоздаёт
	// PerInstanceSMCustomData размером под ТЕКУЩЕЕ число инстансов и зануляет
	// его - вызов на ещё полном компоненте аллоцировал бы большой массив,
	// который ClearInstances() тут же выбросит. Сама она early-out'ит, если
	// значение не изменилось, так что после первого раза бесплатна.
	Comp->SetNumCustomDataFloats(CellCustomDataFloats);
	LastTimings.ClearSeconds = FPlatformTime::Seconds() - ClearStartSeconds;

	// Масштабируем меш так, чтобы его габарит ПО ОСИ X совпал с шагом решётки
	// в плоскости - иначе при несовпадении реального размера меша и шага
	// сетка получается неровной (щели или наложение соседних ячеек).
	//
	// Нормировка именно по одной оси, а не покомпонентная. Покомпонентная
	// принудительно делала мировой габарит инстанса КУБОМ и раздавливала
	// любой меш с неквадратным bounding box - а ячейки Вороного решёток за
	// пределами простой кубической как раз неквадратные (у удлинённого
	// додекаэдра 2:2:3, у гексагональной призмы 1:1.1547:1). Для всех трёх
	// решёток, что были до них, габарит меша кубический, поэтому оба
	// варианта дают тождественно один и тот же масштаб - куб, ромбододекаэдр
	// и усечённый октаэдр выглядят ровно как раньше.
	//
	// Отсюда и точный смысл множителя ниже: CellMeshScaleMultiplier - это
	// ширина клетки по X в единицах шага решётки, а собственные пропорции
	// меша сохраняются как есть.
	const double ScaleStartSeconds = FPlatformTime::Seconds();
	FVector InstanceScale = FVector::OneVector;
	if (const UStaticMesh* MeshPtr = Mesh.Get())
	{
		const double MeshReferenceSize = MeshPtr->GetBounds().BoxExtent.X * 2.0;
		if (!FMath::IsNearlyZero(MeshReferenceSize))
		{
			InstanceScale = FVector(Grid.GetLattice().GetPlanarCellSize() / MeshReferenceSize);
		}
	}
	// Поверх точной подгонки под CellSize - см. SetScaleMultiplier() (1.0
	// для обычных клеток, чуть больше для подсветки выделения).
	InstanceScale *= ScaleMultiplier;
	// Запоминаем: трансформы строятся почанково в AdvanceRenderChunk(), а
	// пересчитывать масштаб там нельзя - он зависит от Grid, ссылку на
	// который между кадрами держать нельзя (Grid подменяется поколением).
	PendingInstanceScale = InstanceScale;
	LastTimings.ScaleSeconds = FPlatformTime::Seconds() - ScaleStartSeconds;

	// Переупорядочиваем инстансы до нарезки на чанки: результат определяет, в
	// каком порядке клетки появляются по кадрам "разлитого" реавила (см.
	// EChunkedRenderOrder). Для одноразового Render() (Order всегда
	// Sequential) это no-op с нулевой стоимостью. Позиции уже мировые, так
	// что компараторы расстояний не зовут виртуальный Grid.GridToWorld()
	// дважды на сравнение, как раньше (~2*N*logN виртуальных вызовов).
	const double ReorderStartSeconds = FPlatformTime::Seconds();
	switch (Order)
	{
	case EChunkedRenderOrder::Sequential:
		break;

	case EChunkedRenderOrder::SequentialReversed:
		Algo::Reverse(PendingInstances);
		break;

	case EChunkedRenderOrder::Shuffled:
		Algo::RandomShuffle(PendingInstances);
		break;

	case EChunkedRenderOrder::DistanceFromCameraNearFirst:
	{
		const FVector3f CameraPos(CameraLocation);
		Algo::Sort(PendingInstances, [&CameraPos](const FCellRenderInstance& A, const FCellRenderInstance& B)
		{
			return FVector3f::DistSquared(A.Position, CameraPos) < FVector3f::DistSquared(B.Position, CameraPos);
		});
		break;
	}

	case EChunkedRenderOrder::DistanceFromCameraFarFirst:
	{
		const FVector3f CameraPos(CameraLocation);
		Algo::Sort(PendingInstances, [&CameraPos](const FCellRenderInstance& A, const FCellRenderInstance& B)
		{
			return FVector3f::DistSquared(A.Position, CameraPos) > FVector3f::DistSquared(B.Position, CameraPos);
		});
		break;
	}

	case EChunkedRenderOrder::FromCenterOutward:
		if (PendingInstances.Num() > 0)
		{
			FVector3f MinPos = PendingInstances[0].Position;
			FVector3f MaxPos = PendingInstances[0].Position;
			for (const FCellRenderInstance& Instance : PendingInstances)
			{
				MinPos = MinPos.ComponentMin(Instance.Position);
				MaxPos = MaxPos.ComponentMax(Instance.Position);
			}
			const FVector3f Center = (MinPos + MaxPos) * 0.5f;
			Algo::Sort(PendingInstances, [&Center](const FCellRenderInstance& A, const FCellRenderInstance& B)
			{
				return FVector3f::DistSquared(A.Position, Center) < FVector3f::DistSquared(B.Position, Center);
			});
		}
		break;
	}
	LastTimings.ReorderSeconds = FPlatformTime::Seconds() - ReorderStartSeconds;

	// Трансформы здесь БОЛЬШЕ НЕ строятся: FTransform под LWC весит 80 байт
	// против 16 у FCellRenderInstance, и держать их все на весь реавил - это
	// 560 МБ против 112 МБ при 7 млн клеток, плюс всплеск на одном кадре.
	// Они строятся почанково в AdvanceRenderChunk(), размазанно по кадрам.

	// Логирование намеренно НЕ здесь: UE_LOG сам по себе (форматирование +
	// запись в файл) стоит времени, и если логировать внутри BeginRender(),
	// эта стоимость попадает в измеряемый снаружи интервал (см. Next()/
	// GenerateRandom() в AutomataOrchestrator), но не в одну из полей
	// LastTimings - разница выглядит как необъяснённый пробел в замерах.
	// Вызывающая сторона логирует один раз, объединяя это с шагом симуляции.
}

bool FInstancedMeshCellGridRenderer::AdvanceRenderChunk(int32 MaxCellsThisChunk)
{
	UInstancedStaticMeshComponent* Comp = Component.Get();
	if (!Comp)
	{
		PendingInstances.Reset();
		PendingCursor = 0;
		return false;
	}

	const int32 Remaining = PendingInstances.Num() - PendingCursor;
	if (Remaining <= 0)
	{
		return false;
	}

	const int32 CountThisChunk = FMath::Min(MaxCellsThisChunk, Remaining);

	// Индекс первого инстанса этого чанка - берём ДО AddInstances(), он же
	// станет началом диапазона для SetCustomData() ниже.
	const int32 FirstInstanceIndex = Comp->GetInstanceCount();

	const double BuildTransformsStartSeconds = FPlatformTime::Seconds();
	ChunkTransformScratch.Reset(CountThisChunk);
	ChunkCustomDataScratch.Reset(CountThisChunk * CellCustomDataFloats);
	for (int32 Index = PendingCursor; Index < PendingCursor + CountThisChunk; ++Index)
	{
		const FCellRenderInstance& Instance = PendingInstances[Index];
		ChunkTransformScratch.Emplace(FQuat::Identity, FVector(Instance.Position), PendingInstanceScale);
		// Обратно в линейный float: цвет клали через ToFColor(bSRGB=false),
		// значит и распаковка линейная, без гамма-коррекции.
		ChunkCustomDataScratch.Add(Instance.Color.R / 255.0f);
		ChunkCustomDataScratch.Add(Instance.Color.G / 255.0f);
		ChunkCustomDataScratch.Add(Instance.Color.B / 255.0f);
	}
	LastTimings.BuildTransformsSeconds += FPlatformTime::Seconds() - BuildTransformsStartSeconds;

	// AddInstance() по одному элементу пересобирает внутренний буфер инстансов
	// на каждый вызов (супралинейный рост при большом числе клеток) - весь
	// чанк уходит одним AddInstances(). bUpdateNavigation=false: навмеш
	// клеткам автомата не нужен.
	const double AddInstanceStartSeconds = FPlatformTime::Seconds();
	Comp->AddInstances(ChunkTransformScratch, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false, /*bUpdateNavigation=*/false);
	LastTimings.AddInstanceSeconds += FPlatformTime::Seconds() - AddInstanceStartSeconds;

	// Конец диапазона ИНКЛЮЗИВНЫЙ - именно это проверяет ensureMsgf внутри
	// SetCustomData(), так что промах на единицу упадёт громко, а не тихо.
	// bMarkRenderStateDirty=false намеренно и MarkRenderStateDirty() тут НЕ
	// зовётся: SetCustomData() сама помечает изменившиеся инстансы через
	// PrimitiveInstanceDataManager (инкрементальный путь под защёлкой), а
	// MarkRenderStateDirty() пересоздал бы SceneProxy целиком - по вызову на
	// чанк это катастрофа. Кадра "инстансы есть, цвет ещё нулевой" не будет:
	// AddInstances() и SetCustomData() происходят в одном вызове с игрового
	// потока, а сбор изменений отложен до конца кадра. При true движок вдобавок
	// зовёт Modify() - запись в транзакцию undo на миллионах инстансов.
	const double CustomDataStartSeconds = FPlatformTime::Seconds();
	Comp->SetCustomData(FirstInstanceIndex, FirstInstanceIndex + CountThisChunk - 1, ChunkCustomDataScratch, /*bMarkRenderStateDirty=*/false);
	LastTimings.CustomDataSeconds += FPlatformTime::Seconds() - CustomDataStartSeconds;

	PendingCursor += CountThisChunk;
	return PendingCursor < PendingInstances.Num();
}
