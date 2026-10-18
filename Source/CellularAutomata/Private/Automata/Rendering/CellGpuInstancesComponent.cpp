#include "Automata/Rendering/CellGpuInstancesComponent.h"

#include "Automata/Simulation/GpuResidentSimulation.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "GPUSceneWriter.h"
#include "InstanceDataSceneProxy.h"
#include "InstancedStaticMeshSceneProxyDesc.h"
#include "NaniteSceneProxy.h"
#include "PrimitiveSceneDesc.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "SceneInterface.h"
#include "ShaderParameterStruct.h"

namespace
{
	/** Цвет экземпляра - те же три float custom data, что HISM даёт M_Cell. */
	constexpr int32 GpuInstanceCustomDataFloats = 3;
	constexpr int32 MaxFaceOffsets = 26;
	/** Меньше не заводим: каждое пересоздание прокси - пара пустых кадров. */
	constexpr int32 MinCellsCapacity = 64 * 1024;
}

/** Параметры отбора видимых (CellVisibility.ush) - общие для счёта и записи. */
BEGIN_SHADER_PARAMETER_STRUCT(FCellVisibilityParameters, )
	SHADER_PARAMETER(FIntVector, InVolumeDim)
	SHADER_PARAMETER(FIntVector, InVolumeMin)
	SHADER_PARAMETER(FVector3f, InStep)
	SHADER_PARAMETER(FVector3f, InOrigin)
	SHADER_PARAMETER(uint32, InFaceOffsetCount)
	SHADER_PARAMETER_ARRAY(FIntVector4, InFaceOffsets, [MaxFaceOffsets])
	SHADER_PARAMETER(uint32, InDecayPlane)
	SHADER_PARAMETER(uint32, InAgeFilterActive)
	SHADER_PARAMETER_ARRAY(FUintVector4, InAgeMask, [2])
	SHADER_PARAMETER(uint32, InCullActive)
	SHADER_PARAMETER(FVector3f, InCullMin)
	SHADER_PARAMETER(FVector3f, InCullMax)
	SHADER_PARAMETER(uint32, InSliceActive)
	SHADER_PARAMETER(FVector3f, InSliceOrigin)
	SHADER_PARAMETER(FVector3f, InSliceForward)
	SHADER_PARAMETER(float, InSliceMinDepth)
	SHADER_PARAMETER(float, InSliceMaxDepth)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputBits)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputAges)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputDecay)
	SHADER_PARAMETER(uint32, InExteriorOnly)
	SHADER_PARAMETER(uint32, InAirRowWords)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputAir)
END_SHADER_PARAMETER_STRUCT()

namespace
{
	/** Air - плоскость внешнего воздуха этого поколения или nullptr (режим выкл.). */
	void FillVisibilityParameters(FRDGBuilder& GraphBuilder, FCellVisibilityParameters& Out, const FCellGpuDrawParams& Params,
		const FGpuResidentPlanes& Planes, FRDGBufferRef Bits, FRDGBufferRef Ages, FRDGBufferRef Decay,
		FRDGBufferRef Air = nullptr, int32 AirRowWords = 0)
	{
		const bool bExterior = Params.bExteriorOnly && Air != nullptr && AirRowWords > 0;
		Out.InExteriorOnly = bExterior ? 1u : 0u;
		Out.InAirRowWords = bExterior ? (uint32)AirRowWords : 0u;
		if (bExterior)
		{
			Out.InputAir = GraphBuilder.CreateSRV(Air);
		}
		else
		{
			// Заглушка: шейдер её не читает, но SRV привязан всегда.
			const uint32 Zero = 0;
			Out.InputAir = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("CellExteriorAirDummy"),
				sizeof(uint32), 1, &Zero, sizeof(uint32)));
		}

		Out.InVolumeDim = Planes.VolumeDim;
		Out.InVolumeMin = Planes.MinCell;
		Out.InStep = Params.Step;
		Out.InOrigin = Params.Origin;
		Out.InFaceOffsetCount = (uint32)FMath::Min(Params.FaceOffsets.Num(), MaxFaceOffsets);
		for (int32 Index = 0; Index < MaxFaceOffsets; ++Index)
		{
			Out.InFaceOffsets[Index] = Params.FaceOffsets.IsValidIndex(Index) ? Params.FaceOffsets[Index] : FIntVector4(0, 0, 0, 0);
		}
		Out.InDecayPlane = Planes.bDecayPlane ? 1u : 0u;
		Out.InAgeFilterActive = Params.bAgeFilterActive ? 1u : 0u;
		Out.InAgeMask[0] = FUintVector4(Params.AgeMask[0], Params.AgeMask[1], Params.AgeMask[2], Params.AgeMask[3]);
		Out.InAgeMask[1] = FUintVector4(Params.AgeMask[4], Params.AgeMask[5], Params.AgeMask[6], Params.AgeMask[7]);
		Out.InCullActive = Params.bCullActive ? 1u : 0u;
		Out.InCullMin = Params.CullMin;
		Out.InCullMax = Params.CullMax;
		Out.InSliceActive = Params.bSliceActive ? 1u : 0u;
		Out.InSliceOrigin = Params.SliceOrigin;
		Out.InSliceForward = Params.SliceForward;
		Out.InSliceMinDepth = Params.SliceMinDepth;
		Out.InSliceMaxDepth = Params.SliceMaxDepth;
		Out.InputBits = GraphBuilder.CreateSRV(Bits);
		Out.InputAges = GraphBuilder.CreateSRV(Ages);
		Out.InputDecay = GraphBuilder.CreateSRV(Decay);
	}

	/** Окружение шейдеров записи в GPU-Scene - те же дефайны, что у PCG
	 *  (PCGSceneWriter.usf): без них шейдер не видит данных примитива. */
	void SetGpuSceneWriterDefines(FShaderCompilerEnvironment& OutEnvironment, uint32 NumThreadsPerGroup)
	{
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("USE_GPU_SCENE_DATA_RW"), 1);
		OutEnvironment.SetDefine(TEXT("NUM_THREADS_PER_GROUP"), NumThreadsPerGroup);
	}
}

/** Тестовый узор прототипа (TestPatternCS). */
class FCellGpuInstanceWriterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellGpuInstanceWriterCS);
	SHADER_USE_PARAMETER_STRUCT(FCellGpuInstanceWriterCS, FGlobalShader);

	static constexpr uint32 NumThreadsPerGroup = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, InPrimitiveId)
		SHADER_PARAMETER(uint32, InNumInstances)
		SHADER_PARAMETER(uint32, InGridDim)
		SHADER_PARAMETER(float, InSpacing)
		SHADER_PARAMETER(float, InMeshScale)
		SHADER_PARAMETER(uint32, InCustomDataCount)
		SHADER_PARAMETER(uint32, InPayloadDataFlags)
		SHADER_PARAMETER_STRUCT_INCLUDE(FGPUSceneWriterParameters, GPUSceneWriterParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		SetGpuSceneWriterDefines(OutEnvironment, NumThreadsPerGroup);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCellGpuInstanceWriterCS, "/Project/CellularAutomata/Private/CellGpuInstanceWriter.usf", "TestPatternCS", SF_Compute);

/** Клетки сессии: отбор видимых и запись экземпляров (CellsCS). */
class FCellGpuCellsWriterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellGpuCellsWriterCS);
	SHADER_USE_PARAMETER_STRUCT(FCellGpuCellsWriterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, InPrimitiveId)
		SHADER_PARAMETER(uint32, InCustomDataCount)
		SHADER_PARAMETER(uint32, InPayloadDataFlags)
		SHADER_PARAMETER(uint32, InCapacity)
		SHADER_PARAMETER(FVector3f, InScale)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCellVisibilityParameters, Visibility)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, AgeLut)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, DecayLut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, RWCounter)
		SHADER_PARAMETER_STRUCT_INCLUDE(FGPUSceneWriterParameters, GPUSceneWriterParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		SetGpuSceneWriterDefines(OutEnvironment, 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCellGpuCellsWriterCS, "/Project/CellularAutomata/Private/CellGpuInstanceWriter.usf", "CellsCS", SF_Compute);

/** Слоты сверх числа видимых - скрыть (HideTailCS). */
class FCellGpuHideTailCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellGpuHideTailCS);
	SHADER_USE_PARAMETER_STRUCT(FCellGpuHideTailCS, FGlobalShader);

	static constexpr uint32 NumThreadsPerGroup = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, InPrimitiveId)
		SHADER_PARAMETER(uint32, InCapacity)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InCounter)
		SHADER_PARAMETER_STRUCT_INCLUDE(FGPUSceneWriterParameters, GPUSceneWriterParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		SetGpuSceneWriterDefines(OutEnvironment, NumThreadsPerGroup);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCellGpuHideTailCS, "/Project/CellularAutomata/Private/CellGpuInstanceWriter.usf", "HideTailCS", SF_Compute);

/** Счёт видимых (CellVisibleCount.usf). */
class FCellVisibleCountCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellVisibleCountCS);
	SHADER_USE_PARAMETER_STRUCT(FCellVisibleCountCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCellVisibilityParameters, Visibility)
		SHADER_PARAMETER(uint32, CounterIndex)
		SHADER_PARAMETER(uint32, HismCounterIndex)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, OutputCounter)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCellVisibleCountCS, "/Project/CellularAutomata/Private/CellVisibleCount.usf", "MainCS", SF_Compute);

void CellGpuDrawing::AddCountVisiblePass(FRDGBuilder& GraphBuilder, const FCellGpuDrawParams& Params,
	const FGpuResidentPlanes& Planes, FRDGBuffer* Bits, FRDGBuffer* Ages, FRDGBuffer* Decay,
	FRDGBufferUAV* Counter, uint32 CounterIndex, FCellGpuDrawState* DrawState)
{
	if (Planes.VolumeDim.X <= 0 || Planes.VolumeDim.Y <= 0 || Planes.VolumeDim.Z <= 0)
	{
		return;
	}

	// Воздух строится здесь, в заходе сессии, и остаётся в DrawState для
	// записи экземпляров - один раз на поколение, а не на кадр.
	FRDGBufferRef Air = nullptr;
	if (Params.bExteriorOnly && DrawState)
	{
		Air = AddExteriorAirPasses(GraphBuilder, Planes, Bits, Decay, *DrawState, Counter,
			CounterIndex + AirRoundsWordOffset, CounterIndex + AirUnconvergedWordOffset);
	}

	FCellVisibleCountCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCellVisibleCountCS::FParameters>();
	FillVisibilityParameters(GraphBuilder, PassParams->Visibility, Params, Planes, Bits, Ages, Decay,
		Air, DrawState ? DrawState->AirRowWords : 0);
	PassParams->CounterIndex = CounterIndex;
	PassParams->HismCounterIndex = CounterIndex + HismVisibleWordOffset;
	PassParams->OutputCounter = Counter;

	TShaderMapRef<FCellVisibleCountCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellVisibleCount"), Shader, PassParams,
		FComputeShaderUtils::GetGroupCount(Planes.VolumeDim, FIntVector(4, 4, 4)));
}

UCellGpuInstancesComponent::UCellGpuInstancesComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	bTickInEditor = true;
	SetMobility(EComponentMobility::Movable);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// GPU-only экземпляры не поддерживают Lumen surface cache и distance
	// fields - движок держит на этом check(), см. ValidateSetup().
	bAffectDynamicIndirectLighting = false;
	bAffectDistanceFieldLighting = false;
}

void UCellGpuInstancesComponent::ShowTestPattern(int32 InGridDim, float InSpacing, float InMeshScale)
{
	bCellsMode = false;
	CellPlanes.Reset();
	CellParams.Reset();
	GridDim = FMath::Max(InGridDim, 0);
	Spacing = InSpacing;
	MeshScale = InMeshScale;
	Capacity = GridDim * GridDim * GridDim;
	WrittenForProxy = nullptr;
	// Новая ёмкость - новый прокси (и потерянные данные): запись произойдёт
	// в TickComponent, когда новый прокси получит слоты в GPU-Scene.
	UpdateBounds();
	MarkRenderStateDirty();
}

void UCellGpuInstancesComponent::ShowResidentCells(const TSharedRef<FGpuResidentPlanes, ESPMode::ThreadSafe>& Planes,
	const TSharedRef<const FCellGpuDrawParams, ESPMode::ThreadSafe>& Params,
	const TSharedRef<FCellGpuDrawState, ESPMode::ThreadSafe>& DrawState,
	int64 VisibleCount, const FBox& LocalBounds, UPrimitiveComponent* InReplacedComponent)
{
	const bool bEnteringCellsMode = !bCellsMode;
	bCellsMode = true;
	CellPlanes = Planes;
	CellParams = Params;
	CellDrawState = DrawState;
	ReplacedComponent = InReplacedComponent;
	bCellsWritePending = true;

	// Прежняя картинка уходит сразу, а не после первой записи: пока на экране
	// оба примитива, в сцене висят и экземпляры HISM, и все слоты GPU-пути.
	// Замер разгона (2026-10-18): 4.7M HISM + 13.1M слотов выбрали видеопамять
	// до предела - кадр на 20 с и несколько по 1-2 с. Пустых кадров выходит
	// три (ожидание слотов в TickComponent), по 8 мс каждый.
	if (bEnteringCellsMode)
	{
		if (UPrimitiveComponent* Replaced = ReplacedComponent.Get())
		{
			Replaced->SetVisibility(false);
		}
	}

	// Ёмкость: впритык с небольшим запасом. Пересоздание прокси стоит
	// пропорционально числу слотов (движок разбирает и собирает их заново в
	// SceneCulling и GPU-Scene), а лишние миллионы слотов - это видеопамять.
	// Вход - x1.25 (первое поколение из шума пористое насквозь и тут же
	// "схлопывается" до поверхности, запас на рост там не нужен), рост - x1.5,
	// ужатие - когда занято меньше трети.
	const int64 Required = FMath::Max<int64>(VisibleCount, 1);
	if (Required > CellGpuDrawing::MaxInstances && !bWarnedOverCeiling)
	{
		bWarnedOverCeiling = true;
		UE_LOG(LogTemp, Warning, TEXT("UCellGpuInstancesComponent: видимых клеток %lld больше потолка %d экземпляров - лишние не рисуются"),
			VisibleCount, CellGpuDrawing::MaxInstances);
	}
	int32 NewCapacity = Capacity;
	if (bEnteringCellsMode)
	{
		NewCapacity = (int32)FMath::Clamp<int64>(Required + Required / 4, MinCellsCapacity, CellGpuDrawing::MaxInstances);
	}
	else if (Required > Capacity || (Capacity > 3 * MinCellsCapacity && Required < Capacity / 3))
	{
		NewCapacity = (int32)FMath::Clamp<int64>(Required + Required / 2, MinCellsCapacity, CellGpuDrawing::MaxInstances);
	}

	// Границы: пересоздаём, только если новый объём вышел за прежние.
	FBox NewBounds = CellBounds;
	if (bEnteringCellsMode || !CellBounds.IsValid || !CellBounds.IsInsideOrOn(LocalBounds.Min) || !CellBounds.IsInsideOrOn(LocalBounds.Max))
	{
		NewBounds = LocalBounds;
	}

	if (NewCapacity != Capacity || !(NewBounds == CellBounds))
	{
		SetCapacityAndBounds(NewCapacity, NewBounds);
	}
}

void UCellGpuInstancesComponent::SetCapacityAndBounds(int32 NewCapacity, const FBox& NewBounds)
{
	if (NewCapacity != Capacity)
	{
		UE_LOG(LogTemp, Log, TEXT("UCellGpuInstancesComponent: ёмкость %d -> %d слотов (прокси пересоздаётся)"), Capacity, NewCapacity);
	}
	Capacity = NewCapacity;
	CellBounds = NewBounds;
	WrittenForProxy = nullptr;
	UpdateBounds();
	MarkRenderStateDirty();
}

void UCellGpuInstancesComponent::ClearCells()
{
	if (!bCellsMode)
	{
		return;
	}
	bCellsMode = false;
	bCellsWritePending = false;
	CellPlanes.Reset();
	CellParams.Reset();
	CellDrawState.Reset();
	CellBounds = FBox(ForceInit);
	Capacity = 0;
	WrittenForProxy = nullptr;
	MarkRenderStateDirty();

	if (UPrimitiveComponent* Replaced = ReplacedComponent.Get())
	{
		Replaced->SetVisibility(true);
	}
	ReplacedComponent.Reset();
}

FBoxSphereBounds UCellGpuInstancesComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// Границы не считаются по экземплярам (их нет на CPU): у клеток сессии
	// это её объём, у узора - куб по GridDim.
	if (bCellsMode && CellBounds.IsValid)
	{
		return FBoxSphereBounds(CellBounds.TransformBy(LocalToWorld));
	}
	const float Half = (FMath::Max(GridDim, 1) * 0.5f + 1.0f) * Spacing;
	const FBox LocalBox(FVector(-Half), FVector(Half));
	return FBoxSphereBounds(LocalBox.TransformBy(LocalToWorld));
}

void UCellGpuInstancesComponent::ValidateSetup()
{
	bAffectDynamicIndirectLighting = false;
	bAffectDistanceFieldLighting = false;
}

FPrimitiveSceneProxy* UCellGpuInstancesComponent::CreateSceneProxy()
{
	if (Capacity <= 0 || !GetStaticMesh())
	{
		return nullptr;
	}
	ValidateSetup();
	return Super::CreateSceneProxy();
}

FPrimitiveSceneProxy* UCellGpuInstancesComponent::CreateStaticMeshSceneProxy(Nanite::FMaterialAudit& NaniteMaterials, bool bCreateNanite)
{
	UWorld* World = GetWorld();
	if (!World || !World->Scene || !UseGPUScene(World->Scene->GetShaderPlatform(), World->Scene->GetFeatureLevel()))
	{
		return nullptr;
	}

	if (!bCreateNanite)
	{
		// Путь рисует только через Nanite (HISM в проекте тоже Nanite).
		UE_LOG(LogTemp, Warning, TEXT("UCellGpuInstancesComponent: меш без Nanite - GPU-экземпляры рисуются только через Nanite"));
		return nullptr;
	}

	FInstanceSceneDataBuffers InstanceSceneDataBuffers(/*InbInstanceDataIsGPUOnly=*/true);
	{
		FInstanceSceneDataBuffers::FAccessTag AccessTag(PointerHash(this));
		FInstanceSceneDataBuffers::FWriteView ProxyData = InstanceSceneDataBuffers.BeginWriteAccess(AccessTag);

		InstanceSceneDataBuffers.SetPrimitiveLocalToWorld(GetRenderMatrix(), AccessTag);

		ProxyData.NumInstancesGPUOnly = Capacity;
		ProxyData.NumCustomDataFloats = GpuInstanceCustomDataFloats;
		ProxyData.InstanceLocalBounds.SetNum(1);
		ProxyData.InstanceLocalBounds[0] = GetStaticMesh()->GetBounds();
		ProxyData.Flags.bHasPerInstanceCustomData = true;
		ProxyData.Flags.bHasPerInstanceRandom = false;

		InstanceSceneDataBuffers.EndWriteAccess(AccessTag);
		InstanceSceneDataBuffers.ValidateData();
	}

	FInstancedStaticMeshSceneProxyDesc Desc;
	Desc.InitializeFromStaticMeshComponent(this);
	Desc.InstanceDataSceneProxy = MakeShared<FInstanceDataSceneProxy, ESPMode::ThreadSafe>(MoveTemp(InstanceSceneDataBuffers));
	Desc.bUseGpuLodSelection = true;

	return ::new Nanite::FSceneProxy(NaniteMaterials, Desc);
}

void UCellGpuInstancesComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!SceneProxy || Capacity <= 0)
	{
		return;
	}

	// Новый прокси: подождать пару кадров, пока GPU-Scene выделит слоты.
	if (SceneProxy != SeenProxy)
	{
		SeenProxy = SceneProxy;
		FramesSinceProxy = 0;
	}
	if (FramesSinceProxy < 3)
	{
		++FramesSinceProxy;
		return;
	}

	if (bCellsMode)
	{
		// Прокси пересоздан (ёмкость, границы, материал) - данные потеряны,
		// пишем заново даже без нового поколения.
		if (SceneProxy != WrittenForProxy)
		{
			bCellsWritePending = true;
		}
		if (!bCellsWritePending)
		{
			return;
		}
		EnqueueCellsWrite();
		bCellsWritePending = false;
		WrittenForProxy = SceneProxy;

		// Картинка на месте - прежний рендер больше не нужен на экране.
		if (UPrimitiveComponent* Replaced = ReplacedComponent.Get())
		{
			if (Replaced->IsVisible())
			{
				Replaced->SetVisibility(false);
			}
		}
		return;
	}

	if (SceneProxy == WrittenForProxy)
	{
		return;
	}
	EnqueueTestPatternWrite();
	WrittenForProxy = SceneProxy;
}

void UCellGpuInstancesComponent::EnqueueCellsWrite()
{
	FSceneInterface* Scene = GetScene();
	if (!Scene || !SceneProxy || !CellPlanes.IsValid() || !CellParams.IsValid() || !CellDrawState.IsValid())
	{
		return;
	}

	FPrimitiveSceneDesc PrimitiveSceneDesc;
	PrimitiveSceneDesc.SceneProxy = SceneProxy;

	TSharedRef<FGpuResidentPlanes, ESPMode::ThreadSafe> Planes = CellPlanes.ToSharedRef();
	TSharedRef<const FCellGpuDrawParams, ESPMode::ThreadSafe> Params = CellParams.ToSharedRef();
	TSharedRef<FCellGpuDrawState, ESPMode::ThreadSafe> DrawState = CellDrawState.ToSharedRef();
	const uint32 InCapacity = (uint32)Capacity;

	PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
	Scene->UpdatePrimitiveInstancesFromCompute(&PrimitiveSceneDesc, FGPUSceneWriteDelegate::CreateLambda(
		[Planes, Params, DrawState, InCapacity](FRDGBuilder& GraphBuilder, const FGPUSceneWriteDelegateParams& WriteParams)
		{
			check(WriteParams.PersistentPrimitiveId != (uint32)INDEX_NONE);

			// Буферы читаются здесь, на render thread, - самое свежее поколение
			// сессии на момент кадра (см. FGpuResidentPlanes).
			const FGpuResidentPlanes& State = *Planes;
			const int32 Cur = State.Current;
			if (!State.Bits[Cur].IsValid() || State.VolumeDim.X <= 0)
			{
				return;
			}

			FRDGBufferRef Bits = GraphBuilder.RegisterExternalBuffer(State.Bits[Cur]);
			FRDGBufferRef Ages = GraphBuilder.RegisterExternalBuffer(State.Ages[Cur]);
			FRDGBufferRef Decay = GraphBuilder.RegisterExternalBuffer(State.Decay[Cur]);

			FRDGBufferRef Counter = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("CellGpuVisibleCounter"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Counter, PF_R32_UINT), 0u);

			FRDGBufferRef AgeLut = CreateStructuredBuffer(GraphBuilder, TEXT("CellGpuAgeLut"), sizeof(uint32), Params->AgeLut.Num(),
				Params->AgeLut.GetData(), Params->AgeLut.Num() * sizeof(uint32));
			FRDGBufferRef DecayLut = CreateStructuredBuffer(GraphBuilder, TEXT("CellGpuDecayLut"), sizeof(uint32), Params->DecayLut.Num(),
				Params->DecayLut.GetData(), Params->DecayLut.Num() * sizeof(uint32));

			FCellGpuCellsWriterCS::FParameters* CellsParams = GraphBuilder.AllocParameters<FCellGpuCellsWriterCS::FParameters>();
			CellsParams->InPrimitiveId = WriteParams.PersistentPrimitiveId;
			CellsParams->InCustomDataCount = WriteParams.NumCustomDataFloats;
			CellsParams->InPayloadDataFlags = WriteParams.PackedInstanceSceneDataFlags;
			CellsParams->InCapacity = InCapacity;
			CellsParams->InScale = Params->Scale;
			// Воздух - тот, что построил заход для этих же плоскостей; объём не
			// совпал (заход ещё не доехал) - рисуем по "только поверхности".
			FRDGBufferRef Air = nullptr;
			if (Params->bExteriorOnly && DrawState->Air.IsValid() && DrawState->AirDim == State.VolumeDim)
			{
				Air = GraphBuilder.RegisterExternalBuffer(DrawState->Air);
			}
			FillVisibilityParameters(GraphBuilder, CellsParams->Visibility, *Params, State, Bits, Ages, Decay,
				Air, DrawState->AirRowWords);
			CellsParams->AgeLut = GraphBuilder.CreateSRV(AgeLut);
			CellsParams->DecayLut = GraphBuilder.CreateSRV(DecayLut);
			CellsParams->RWCounter = GraphBuilder.CreateUAV(Counter, PF_R32_UINT);
			// PCG в 5.7 пишет через ту же (помеченную устаревшей) структуру -
			// повторяем проверенный путь, а не новый UB без примера.
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			CellsParams->GPUSceneWriterParameters = WriteParams.GPUWriteParams;
			PRAGMA_ENABLE_DEPRECATION_WARNINGS

			TShaderMapRef<FCellGpuCellsWriterCS> CellsShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellGpuCellsWriter"),
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, CellsShader, CellsParams,
				FComputeShaderUtils::GetGroupCount(State.VolumeDim, FIntVector(4, 4, 4)));

			FCellGpuHideTailCS::FParameters* TailParams = GraphBuilder.AllocParameters<FCellGpuHideTailCS::FParameters>();
			TailParams->InPrimitiveId = WriteParams.PersistentPrimitiveId;
			TailParams->InCapacity = InCapacity;
			TailParams->InCounter = GraphBuilder.CreateSRV(Counter);
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			TailParams->GPUSceneWriterParameters = WriteParams.GPUWriteParams;
			PRAGMA_ENABLE_DEPRECATION_WARNINGS

			TShaderMapRef<FCellGpuHideTailCS> TailShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellGpuHideTail"),
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, TailShader, TailParams,
				FComputeShaderUtils::GetGroupCountWrapped(FMath::DivideAndRoundUp<int32>((int32)InCapacity, FCellGpuHideTailCS::NumThreadsPerGroup)));
		}));
	PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
}

void UCellGpuInstancesComponent::EnqueueTestPatternWrite()
{
	FSceneInterface* Scene = GetScene();
	if (!Scene || !SceneProxy)
	{
		return;
	}

	FPrimitiveSceneDesc PrimitiveSceneDesc;
	PrimitiveSceneDesc.SceneProxy = SceneProxy;

	const uint32 NumInstances = (uint32)Capacity;
	const uint32 Dim = (uint32)GridDim;
	const float InSpacing = Spacing;
	const float InMeshScale = MeshScale;

	PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
	Scene->UpdatePrimitiveInstancesFromCompute(&PrimitiveSceneDesc, FGPUSceneWriteDelegate::CreateLambda(
		[NumInstances, Dim, InSpacing, InMeshScale](FRDGBuilder& GraphBuilder, const FGPUSceneWriteDelegateParams& Params)
		{
			check(Params.PersistentPrimitiveId != (uint32)INDEX_NONE);

			FCellGpuInstanceWriterCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCellGpuInstanceWriterCS::FParameters>();
			Parameters->InPrimitiveId = Params.PersistentPrimitiveId;
			Parameters->InNumInstances = NumInstances;
			Parameters->InGridDim = Dim;
			Parameters->InSpacing = InSpacing;
			Parameters->InMeshScale = InMeshScale;
			Parameters->InCustomDataCount = Params.NumCustomDataFloats;
			Parameters->InPayloadDataFlags = Params.PackedInstanceSceneDataFlags;
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			Parameters->GPUSceneWriterParameters = Params.GPUWriteParams;
			PRAGMA_ENABLE_DEPRECATION_WARNINGS

			TShaderMapRef<FCellGpuInstanceWriterCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(
				FMath::DivideAndRoundUp<int32>((int32)NumInstances, FCellGpuInstanceWriterCS::NumThreadsPerGroup));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellGpuInstanceWriter"),
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, Shader, Parameters, GroupCount);
		}));
	PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS

	UE_LOG(LogTemp, Log, TEXT("UCellGpuInstancesComponent: запись %u экземпляров (%u^3) поставлена в обновление GPU-Scene"), NumInstances, Dim);
}

ACellGpuInstancesTestActor::ACellGpuInstancesTestActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Instances = CreateDefaultSubobject<UCellGpuInstancesComponent>(TEXT("Instances"));
	RootComponent = Instances;
}
