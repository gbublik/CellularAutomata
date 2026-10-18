#include "Automata/Rendering/CellGpuInstancesComponent.h"

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

/** Пишет экземпляры тестового узора прямо в GPU-Scene. Повторяет
 *  FPCGSceneWriterCS (Plugins/PCG/Shaders/Private/PCGSceneWriter.usf):
 *  те же дефайны окружения и та же структура параметров записи. */
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
		// Те же три дефайна, что у PCG: без первых двух шейдер не видит
		// данных примитива и номера кадра GPU-Scene.
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("USE_GPU_SCENE_DATA_RW"), 1);
		OutEnvironment.SetDefine(TEXT("NUM_THREADS_PER_GROUP"), NumThreadsPerGroup);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCellGpuInstanceWriterCS, "/Project/CellularAutomata/Private/CellGpuInstanceWriter.usf", "MainCS", SF_Compute);

namespace
{
	/** Цвет экземпляра - те же три float custom data, что HISM даёт M_Cell. */
	constexpr int32 GpuInstanceCustomDataFloats = 3;
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

FBoxSphereBounds UCellGpuInstancesComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// Границы не считаются по экземплярам (их нет на CPU) - задаются узором.
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
		// Прототип проверяет именно Nanite-путь; без Nanite-меша сравнивать
		// не с чем (HISM в проекте рисует через Nanite).
		UE_LOG(LogTemp, Warning, TEXT("UCellGpuInstancesComponent: меш без Nanite - прототип рисует только через Nanite"));
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

	if (!SceneProxy || Capacity <= 0 || SceneProxy == WrittenForProxy)
	{
		return;
	}

	// Новый прокси: подождать пару кадров, пока GPU-Scene выделит слоты.
	if (SceneProxy != SeenProxy)
	{
		SeenProxy = SceneProxy;
		FramesSinceProxy = 0;
	}
	if (++FramesSinceProxy < 3)
	{
		return;
	}

	EnqueueWrite();
	WrittenForProxy = SceneProxy;
}

void UCellGpuInstancesComponent::EnqueueWrite()
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
			// PCG в 5.7 пишет через ту же (помеченную устаревшей) структуру -
			// повторяем проверенный путь, а не новый UB без примера.
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
