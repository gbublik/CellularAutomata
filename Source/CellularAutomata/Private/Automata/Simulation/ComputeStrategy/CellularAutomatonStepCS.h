#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

/** Шейдер одного поколения (CellularAutomatonStep.usf). Объявление вынесено
 *  сюда из GpuComputeStrategy.cpp, потому что им пользуются двое: стратегия
 *  (круг CPU -> GPU -> CPU на заход) и FGpuResidentSimulation (состояние
 *  живёт на GPU между заходами). IMPLEMENT_GLOBAL_SHADER остаётся один - в
 *  GpuComputeStrategy.cpp. */
namespace CellularAutomatonStepShader
{
	// Moore - максимум соседей (полный куб 3x3x3 без центра), см.
	// CellularAutomatonRule.cpp::BuildNeighborOffsets.
	constexpr int32 MaxNeighborOffsets = 26;
}

class FCellularAutomatonStepCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellularAutomatonStepCS);
	SHADER_USE_PARAMETER_STRUCT(FCellularAutomatonStepCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, VolumeDim)
		SHADER_PARAMETER(uint32, NeighborOffsetCount)
		SHADER_PARAMETER_ARRAY(FIntVector4, NeighborOffsets, [CellularAutomatonStepShader::MaxNeighborOffsets])
		SHADER_PARAMETER(uint32, BirthMask)
		SHADER_PARAMETER(uint32, SurvivalMask)
		// bDecayActive==0 (States==2, подавляющее большинство правил) -
		// InputDecayBits привязывается на тот же RDG-буфер, что InputBits -
		// шейдер его не разыменовывает под этой веткой, так что лишнего
		// аллоцирования не происходит.
		SHADER_PARAMETER(uint32, bDecayActive)
		// bTrackAges==1 - возраст ведёт шейдер (пачка, либо GPU-сессия, где
		// промежуточных поколений на CPU нет вовсе). ==0 - возрасты после шага
		// считает CPU; InputAges/OutputAges тогда привязаны к заглушкам.
		SHADER_PARAMETER(uint32, bTrackAges)
		// bTrackDecayStates==1 - угасание ведёт шейдер по байтовой плоскости
		// состояний. ==0 - прежний путь с битовой плоскостью InputDecayBits.
		SHADER_PARAMETER(uint32, bTrackDecayStates)
		SHADER_PARAMETER(uint32, MaxStates)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputBits)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputDecayBits)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputAges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputDecayStates)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, OutputBits)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, OutputAges)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, OutputDecayStates)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
