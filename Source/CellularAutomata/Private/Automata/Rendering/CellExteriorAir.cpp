// Проходы "внешнего воздуха" (CellExteriorAir.usf) - что видно снаружи.

#include "Automata/Rendering/CellGpuInstancesComponent.h"
#include "Automata/Simulation/GpuResidentSimulation.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

namespace
{
	/** Раундов дотекания (по четыре прохода) за поколение. Сошедшаяся заливка
	 *  гасит остаток раундов до нуля групп, так что потолок - про худший
	 *  случай, а не про цену. Не сошлась - флаг в сводке и строка в логе. */
	constexpr int32 MaxPropagateRounds = 64;
	constexpr int32 PassesPerRound = 4;

	bool ShouldCompileAirShader(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
}

#define CA_AIR_SHADER_BOILERPLATE(ClassName) \
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return ShouldCompileAirShader(Parameters); }

class FCellAirBuildEmptyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellAirBuildEmptyCS);
	SHADER_USE_PARAMETER_STRUCT(FCellAirBuildEmptyCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, VolumeDim)
		SHADER_PARAMETER(uint32, RowWords)
		SHADER_PARAMETER(uint32, AirWordCount)
		SHADER_PARAMETER(uint32, bDecayPlane)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputBits)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputDecay)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, RWEmpty)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, RWAir)
	END_SHADER_PARAMETER_STRUCT()

	CA_AIR_SHADER_BOILERPLATE(FCellAirBuildEmptyCS)
};
IMPLEMENT_GLOBAL_SHADER(FCellAirBuildEmptyCS, "/Project/CellularAutomata/Private/CellExteriorAir.usf", "BuildEmptyCS", SF_Compute);

BEGIN_SHADER_PARAMETER_STRUCT(FCellAirSweepParameters, )
	SHADER_PARAMETER(FIntVector, VolumeDim)
	SHADER_PARAMETER(uint32, RowWords)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, EmptyPlane)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, RWAir)
END_SHADER_PARAMETER_STRUCT()

class FCellAirSweepXCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellAirSweepXCS);
	using FParameters = FCellAirSweepParameters;
	SHADER_USE_PARAMETER_STRUCT(FCellAirSweepXCS, FGlobalShader);
	CA_AIR_SHADER_BOILERPLATE(FCellAirSweepXCS)
};
IMPLEMENT_GLOBAL_SHADER(FCellAirSweepXCS, "/Project/CellularAutomata/Private/CellExteriorAir.usf", "SweepXCS", SF_Compute);

class FCellAirSweepYCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellAirSweepYCS);
	using FParameters = FCellAirSweepParameters;
	SHADER_USE_PARAMETER_STRUCT(FCellAirSweepYCS, FGlobalShader);
	CA_AIR_SHADER_BOILERPLATE(FCellAirSweepYCS)
};
IMPLEMENT_GLOBAL_SHADER(FCellAirSweepYCS, "/Project/CellularAutomata/Private/CellExteriorAir.usf", "SweepYCS", SF_Compute);

class FCellAirSweepZCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellAirSweepZCS);
	using FParameters = FCellAirSweepParameters;
	SHADER_USE_PARAMETER_STRUCT(FCellAirSweepZCS, FGlobalShader);
	CA_AIR_SHADER_BOILERPLATE(FCellAirSweepZCS)
};
IMPLEMENT_GLOBAL_SHADER(FCellAirSweepZCS, "/Project/CellularAutomata/Private/CellExteriorAir.usf", "SweepZCS", SF_Compute);

class FCellAirPropagateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellAirPropagateCS);
	SHADER_USE_PARAMETER_STRUCT(FCellAirPropagateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, VolumeDim)
		SHADER_PARAMETER(uint32, RowWords)
		SHADER_PARAMETER(uint32, AirWordCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, EmptyPlane)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, RWAir)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, RWChanged)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	CA_AIR_SHADER_BOILERPLATE(FCellAirPropagateCS)
};
IMPLEMENT_GLOBAL_SHADER(FCellAirPropagateCS, "/Project/CellularAutomata/Private/CellExteriorAir.usf", "PropagateCS", SF_Compute);

class FCellAirGateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellAirGateCS);
	SHADER_USE_PARAMETER_STRUCT(FCellAirGateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector2, PropagateGroups)
		SHADER_PARAMETER(uint32, StatsRoundsWord)
		SHADER_PARAMETER(uint32, StatsUnconvergedWord)
		SHADER_PARAMETER(uint32, bLastGate)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, RWChanged)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint32>, RWIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, RWStats)
	END_SHADER_PARAMETER_STRUCT()

	CA_AIR_SHADER_BOILERPLATE(FCellAirGateCS)
};
IMPLEMENT_GLOBAL_SHADER(FCellAirGateCS, "/Project/CellularAutomata/Private/CellExteriorAir.usf", "GateCS", SF_Compute);

#undef CA_AIR_SHADER_BOILERPLATE

namespace
{
	template <typename ShaderType>
	void AddSweepPass(FRDGBuilder& GraphBuilder, FGlobalShaderMap* GlobalShaders, const FIntVector& Dim, uint32 RowWords,
		FRDGBufferRef Empty, FRDGBufferRef Air, int32 Threads)
	{
		FCellAirSweepParameters* Params = GraphBuilder.AllocParameters<FCellAirSweepParameters>();
		Params->VolumeDim = Dim;
		Params->RowWords = RowWords;
		Params->EmptyPlane = GraphBuilder.CreateSRV(Empty);
		Params->RWAir = GraphBuilder.CreateUAV(Air);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellAirSweep"),
			TShaderMapRef<ShaderType>(GlobalShaders), Params,
			FComputeShaderUtils::GetGroupCountWrapped(FMath::DivideAndRoundUp(Threads, 64)));
	}
}

FRDGBuffer* CellGpuDrawing::AddExteriorAirPasses(FRDGBuilder& GraphBuilder, const FGpuResidentPlanes& Planes,
	FRDGBuffer* Bits, FRDGBuffer* Decay, FCellGpuDrawState& DrawState,
	FRDGBufferUAV* Stats, uint32 RoundsWord, uint32 UnconvergedWord)
{
	const FIntVector Dim = Planes.VolumeDim;
	if (Dim.X <= 0 || Dim.Y <= 0 || Dim.Z <= 0)
	{
		return nullptr;
	}
	const uint32 RowWords = (uint32)FMath::DivideAndRoundUp(Dim.X, 32);
	const int64 AirWords64 = int64(RowWords) * Dim.Y * Dim.Z;
	if (AirWords64 > MAX_int32)
	{
		return nullptr;
	}
	const int32 AirWords = (int32)AirWords64;

	// Плоскость воздуха живёт между кадрами: её читает запись экземпляров,
	// которая идёт в графе сцены, а не здесь. Пересоздаётся при смене объёма.
	FRDGBufferRef Air;
	if (DrawState.Air.IsValid() && DrawState.AirDim == Dim)
	{
		Air = GraphBuilder.RegisterExternalBuffer(DrawState.Air);
	}
	else
	{
		Air = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), AirWords), TEXT("CellExteriorAir"));
	}
	FRDGBufferRef Empty = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), AirWords), TEXT("CellExteriorEmpty"));
	FGlobalShaderMap* GlobalShaders = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	const FIntVector WordGroups = FComputeShaderUtils::GetGroupCountWrapped(FMath::DivideAndRoundUp(AirWords, 64));

	{
		FCellAirBuildEmptyCS::FParameters* Params = GraphBuilder.AllocParameters<FCellAirBuildEmptyCS::FParameters>();
		Params->VolumeDim = Dim;
		Params->RowWords = RowWords;
		Params->AirWordCount = (uint32)AirWords;
		Params->bDecayPlane = Planes.bDecayPlane ? 1u : 0u;
		Params->InputBits = GraphBuilder.CreateSRV(Bits);
		Params->InputDecay = GraphBuilder.CreateSRV(Decay);
		Params->RWEmpty = GraphBuilder.CreateUAV(Empty);
		Params->RWAir = GraphBuilder.CreateUAV(Air);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellAirBuildEmpty"),
			TShaderMapRef<FCellAirBuildEmptyCS>(GlobalShaders), Params, WordGroups);
	}

	AddSweepPass<FCellAirSweepXCS>(GraphBuilder, GlobalShaders, Dim, RowWords, Empty, Air, Dim.Y * Dim.Z);
	AddSweepPass<FCellAirSweepYCS>(GraphBuilder, GlobalShaders, Dim, RowWords, Empty, Air, (int32)RowWords * Dim.Z);
	AddSweepPass<FCellAirSweepZCS>(GraphBuilder, GlobalShaders, Dim, RowWords, Empty, Air, (int32)RowWords * Dim.Y);

	// Дотекание раундами. Флаг изменения поднят заранее - первый Gate выдаёт
	// полные аргументы.
	FRDGBufferRef Changed = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("CellExteriorChanged"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Changed), 1u);
	FRDGBufferRef IndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("CellExteriorArgs"));

	auto AddGate = [&](bool bLast)
	{
		FCellAirGateCS::FParameters* Params = GraphBuilder.AllocParameters<FCellAirGateCS::FParameters>();
		Params->PropagateGroups = FUintVector2((uint32)WordGroups.X, (uint32)WordGroups.Y);
		Params->StatsRoundsWord = RoundsWord;
		Params->StatsUnconvergedWord = UnconvergedWord;
		Params->bLastGate = bLast ? 1u : 0u;
		Params->RWChanged = GraphBuilder.CreateUAV(Changed);
		Params->RWIndirectArgs = GraphBuilder.CreateUAV(IndirectArgs, PF_R32_UINT);
		Params->RWStats = Stats;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellAirGate"),
			TShaderMapRef<FCellAirGateCS>(GlobalShaders), Params, FIntVector(1, 1, 1));
	};

	TShaderMapRef<FCellAirPropagateCS> PropagateShader(GlobalShaders);
	for (int32 Round = 0; Round < MaxPropagateRounds; ++Round)
	{
		AddGate(/*bLast=*/false);
		for (int32 Pass = 0; Pass < PassesPerRound; ++Pass)
		{
			FCellAirPropagateCS::FParameters* Params = GraphBuilder.AllocParameters<FCellAirPropagateCS::FParameters>();
			Params->VolumeDim = Dim;
			Params->RowWords = RowWords;
			Params->AirWordCount = (uint32)AirWords;
			Params->EmptyPlane = GraphBuilder.CreateSRV(Empty);
			Params->RWAir = GraphBuilder.CreateUAV(Air);
			Params->RWChanged = GraphBuilder.CreateUAV(Changed);
			Params->IndirectArgs = IndirectArgs;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellAirPropagate"),
				PropagateShader, Params, IndirectArgs, 0);
		}
	}
	AddGate(/*bLast=*/true);

	DrawState.Air = GraphBuilder.ConvertToExternalBuffer(Air);
	DrawState.AirDim = Dim;
	DrawState.AirRowWords = (int32)RowWords;
	return Air;
}

FCellGpuDrawState::~FCellGpuDrawState()
{
	if (Air.IsValid() && !IsInRenderingThread())
	{
		ENQUEUE_RENDER_COMMAND(CellGpuDrawStateRelease)(
			[Buffer = MoveTemp(Air)](FRHICommandListImmediate&) mutable
			{
				Buffer.SafeRelease();
			});
	}
}
