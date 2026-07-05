#include "Automata/Simulation/ComputeStrategy/GpuComputeStrategy.h"
#include "Automata/Simulation/ComputeStrategy/CpuComputeStrategy.h"
#include "Automata/Grid/CellGrid.h"
#include "Automata/Simulation/CellularAutomatonRule.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "HAL/Event.h"

namespace
{
	// Moore - максимум соседей (полный куб 3x3x3 без центра), см.
	// CellularAutomatonRule.cpp::BuildNeighborOffsets.
	constexpr int32 MaxShaderNeighborOffsets = 26;
}

class FCellularAutomatonStepCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellularAutomatonStepCS);
	SHADER_USE_PARAMETER_STRUCT(FCellularAutomatonStepCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, VolumeDim)
		SHADER_PARAMETER(uint32, NeighborOffsetCount)
		SHADER_PARAMETER_ARRAY(FIntVector4, NeighborOffsets, [MaxShaderNeighborOffsets])
		SHADER_PARAMETER(uint32, BirthMask)
		SHADER_PARAMETER(uint32, SurvivalMask)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputBits)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, OutputBits)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCellularAutomatonStepCS, "/Project/CellularAutomata/Private/CellularAutomatonStep.usf", "MainCS", SF_Compute);

void FGpuComputeStrategy::Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule) const
{
	TArray<FIntVector> AliveCells;
	CurrentGrid.GetAliveCells(AliveCells);

	if (AliveCells.Num() == 0)
	{
		return;
	}

	const TArray<FIntVector>& NeighborOffsets = Rule.GetNeighborOffsets();

	FIntVector MinCell = AliveCells[0];
	FIntVector MaxCell = AliveCells[0];
	for (const FIntVector& Cell : AliveCells)
	{
		MinCell.X = FMath::Min(MinCell.X, Cell.X);
		MinCell.Y = FMath::Min(MinCell.Y, Cell.Y);
		MinCell.Z = FMath::Min(MinCell.Z, Cell.Z);
		MaxCell.X = FMath::Max(MaxCell.X, Cell.X);
		MaxCell.Y = FMath::Max(MaxCell.Y, Cell.Y);
		MaxCell.Z = FMath::Max(MaxCell.Z, Cell.Z);
	}

	// Halo = 1: все NeighborOffsets (VonNeumann/Moore) имеют компоненты
	// только в {-1,0,1} - см. CellularAutomatonRule.cpp::BuildNeighborOffsets.
	constexpr int32 Halo = 1;
	MinCell -= FIntVector(Halo, Halo, Halo);
	MaxCell += FIntVector(Halo, Halo, Halo);

	const FIntVector VolumeDim = MaxCell - MinCell + FIntVector(1, 1, 1);
	const int64 VolumeCells = int64(VolumeDim.X) * int64(VolumeDim.Y) * int64(VolumeDim.Z);

	// Защита от OOM: две далёкие друг от друга живые клетки в разреженной
	// сетке могли бы раздуть AABB до неподъёмного объёма - в этом случае
	// откатываемся на CPU вместо аллокации гигантского битового буфера.
	constexpr int64 MaxVolumeCells = 256ll * 256ll * 256ll;
	if (VolumeCells <= 0 || VolumeCells > MaxVolumeCells)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGpuComputeStrategy::Step: ограничивающий объём (%lld клеток) превышает лимит GPU-буфера - fallback на CPU"), VolumeCells);
		FCpuComputeStrategy CpuFallback;
		CpuFallback.Step(CurrentGrid, NextGrid, Rule);
		return;
	}

	const int32 VolumeCellsI32 = (int32)VolumeCells;
	const int32 WordCount = FMath::DivideAndRoundUp(VolumeCellsI32, 32);

	TArray<uint32> InputWords;
	InputWords.SetNumZeroed(WordCount);
	for (const FIntVector& Cell : AliveCells)
	{
		const FIntVector Local = Cell - MinCell;
		const int32 Index = (Local.Z * VolumeDim.Y + Local.Y) * VolumeDim.X + Local.X;
		InputWords[Index >> 5] |= (1u << (Index & 31));
	}

	TArray<FIntVector4> ShaderOffsets;
	ShaderOffsets.SetNumZeroed(MaxShaderNeighborOffsets);
	for (int32 Index = 0; Index < NeighborOffsets.Num(); ++Index)
	{
		const FIntVector& Offset = NeighborOffsets[Index];
		ShaderOffsets[Index] = FIntVector4(Offset.X, Offset.Y, Offset.Z, 0);
	}

	uint32 BirthMask = 0;
	for (int32 Count : Rule.GetBirthCounts())
	{
		if (Count >= 0 && Count < 32)
		{
			BirthMask |= (1u << Count);
		}
	}
	uint32 SurvivalMask = 0;
	for (int32 Count : Rule.GetSurvivalCounts())
	{
		if (Count >= 0 && Count < 32)
		{
			SurvivalMask |= (1u << Count);
		}
	}

	const int32 NeighborOffsetCount = NeighborOffsets.Num();

	// Диспатч, readback-опрос и Lock/Unlock идут ЦЕЛИКОМ на Render Thread -
	// FRHIGPUBufferReadback::IsReady()/Poll() не рассчитаны на вызов с
	// произвольного потока (наблюдался краш "Array index out of bounds: 0 into
	// an array of size 0" внутри Poll(), когда IsReady() опрашивался с
	// ThreadPool-воркера StepAsync()). Вызывающий поток (game thread из Next()
	// либо ThreadPool worker из StepAsync()) блокируется на raw FEvent, а не
	// на FRenderCommandFence - у последней Wait()/IsFenceComplete() сами
	// содержат check(IsInGameThread() || IsInAsyncLoadingThread()) и падают
	// именно на ThreadPool-воркере StepAsync() (тоже наблюдалось на
	// практике). FEvent из пула - обычный ОС-примитив без такого
	// ограничения. Сам результат уходит через TSharedRef<TArray<uint32>>,
	// которую до Event->Wait() трогает только Render Thread.
	TSharedRef<TArray<uint32>> OutputWords = MakeShared<TArray<uint32>>();
	OutputWords->SetNumZeroed(WordCount);

	FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(false);
	ENQUEUE_RENDER_COMMAND(CellularAutomatonStepCommand)(
		[InputWords = MoveTemp(InputWords), ShaderOffsets = MoveTemp(ShaderOffsets), WordCount, VolumeDim,
		 NeighborOffsetCount, BirthMask, SurvivalMask, OutputWords, CompletionEvent](FRHICommandListImmediate& RHICmdList) mutable
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGBufferRef InputBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("CAInputBits"),
				sizeof(uint32), WordCount, InputWords.GetData(), WordCount * sizeof(uint32));

			FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), WordCount), TEXT("CAOutputBits"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutputBuffer, PF_R32_UINT), 0u);

			FCellularAutomatonStepCS::FParameters* Params = GraphBuilder.AllocParameters<FCellularAutomatonStepCS::FParameters>();
			Params->VolumeDim = VolumeDim;
			Params->NeighborOffsetCount = (uint32)NeighborOffsetCount;
			for (int32 Index = 0; Index < MaxShaderNeighborOffsets; ++Index)
			{
				Params->NeighborOffsets[Index] = ShaderOffsets[Index];
			}
			Params->BirthMask = BirthMask;
			Params->SurvivalMask = SurvivalMask;
			Params->InputBits = GraphBuilder.CreateSRV(InputBuffer);
			Params->OutputBits = GraphBuilder.CreateUAV(OutputBuffer, PF_R32_UINT);

			TShaderMapRef<FCellularAutomatonStepCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellularAutomatonStep"),
				ComputeShader, Params, FComputeShaderUtils::GetGroupCount(VolumeDim, FIntVector(4, 4, 4)));

			TSharedRef<FRHIGPUBufferReadback> Readback = MakeShared<FRHIGPUBufferReadback>(TEXT("CellularAutomatonStepReadback"));
			AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CellularAutomatonStepReadback"), OutputBuffer,
				[Readback, OutputBuffer](FRHICommandListImmediate& RHICmdListLocal)
				{
					Readback->EnqueueCopy(RHICmdListLocal, OutputBuffer->GetRHI(), 0);
				});

			GraphBuilder.Execute();

			// Лёгкий flush - проталкивает записанные команды до RHI thread (и тем
			// самым до GPU), не дожидаясь их завершения. Без него команда так и
			// остаётся в непроталкнутом RHICmdList, пока мы сами же (Render Thread)
			// сидим в цикле ниже - GPU физически нечего сигналить, и опрос
			// Readback->IsReady() зависает навсегда (наблюдалось на практике:
			// весь редактор подвисал при Start() с ComputeMethod=Gpu). Это НЕ
			// SubmitAndBlockUntilGPUIdle()/устаревший SubmitCommandsAndFlushGPU() -
			// тот дожидается простоя ВСЕГО GPU-конвейера и вызывал отдельный краш
			// (детектор зависания потока, ~6с на тривиальном диспатче). FlushRHIThread
			// только отправляет команды, не блокируясь на их выполнении GPU.
			RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

			// Опрос готовности - здесь безопасно, мы уже на Render Thread (см.
			// комментарий выше про причину переноса всего блока сюда).
			while (!Readback->IsReady())
			{
				FPlatformProcess::Sleep(0.0f);
			}

			const uint32* ResultData = (const uint32*)Readback->Lock(WordCount * sizeof(uint32));
			FMemory::Memcpy(OutputWords->GetData(), ResultData, WordCount * sizeof(uint32));
			Readback->Unlock();

			CompletionEvent->Trigger();
		});

	CompletionEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);

	const int32 SliceCells = VolumeDim.X * VolumeDim.Y;
	for (int32 Index = 0; Index < VolumeCellsI32; ++Index)
	{
		if (((*OutputWords)[Index >> 5] >> (Index & 31)) & 1u)
		{
			const int32 LocalZ = Index / SliceCells;
			const int32 Rem = Index % SliceCells;
			const int32 LocalY = Rem / VolumeDim.X;
			const int32 LocalX = Rem % VolumeDim.X;
			NextGrid.SetAlive(MinCell + FIntVector(LocalX, LocalY, LocalZ), true);
		}
	}
}
