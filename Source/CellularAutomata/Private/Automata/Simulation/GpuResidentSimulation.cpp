#include "Automata/Simulation/GpuResidentSimulation.h"
#include "Automata/Grid/CellGrid.h"
#include "Automata/Simulation/CellularAutomatonRule.h"
#include "ComputeStrategy/CellularAutomatonStepCS.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/Event.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"

/** Сводка по объёму (CellularAutomatonStats.usf): число живых и габарит. */
class FCellularAutomatonStatsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellularAutomatonStatsCS);
	SHADER_USE_PARAMETER_STRUCT(FCellularAutomatonStatsCS, FGlobalShader);

	static constexpr int32 GroupSize = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, VolumeDim)
		SHADER_PARAMETER(uint32, WordCount)
		SHADER_PARAMETER(uint32, bDecayPlane)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputBits)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputDecay)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, OutputStats)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCellularAutomatonStatsCS, "/Project/CellularAutomata/Private/CellularAutomatonStats.usf", "MainCS", SF_Compute);

/** Перекладка плоскостей в новую раскладку (CellularAutomatonRelayout.usf). */
class FCellularAutomatonRelayoutCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCellularAutomatonRelayoutCS);
	SHADER_USE_PARAMETER_STRUCT(FCellularAutomatonRelayoutCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, NewDim)
		SHADER_PARAMETER(FIntVector, OldDim)
		SHADER_PARAMETER(FIntVector, OldOffsetInNew)
		SHADER_PARAMETER(uint32, bDecayPlane)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputBits)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputAges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InputDecay)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, OutputBits)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, OutputAges)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, OutputDecay)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCellularAutomatonRelayoutCS, "/Project/CellularAutomata/Private/CellularAutomatonRelayout.usf", "MainCS", SF_Compute);

namespace
{
	/** Слов в сводке (CellularAutomatonStats.usf, раскладка - в его шапке). */
	constexpr int32 StatsWordCount = 20;
}

/** Плоскости на CPU: биты жизни (по биту), возрасты и стадии угасания (по
 *  байту, упакованы по четыре в слово) - та же раскладка, что у шейдера. */
struct FGpuResidentSimulation::FPlanes
{
	TArray<uint32> Bits;
	TArray<uint32> Ages;
	TArray<uint32> Decay;
};

namespace
{
	int32 BitWords(int64 Cells) { return (int32)FMath::DivideAndRoundUp<int64>(Cells, 32); }
	int32 ByteWords(int64 Cells) { return (int32)FMath::DivideAndRoundUp<int64>(Cells, 4); }

	int32 LinearIndex(const FIntVector& Local, const FIntVector& Dim)
	{
		return (Local.Z * Dim.Y + Local.Y) * Dim.X + Local.X;
	}

	uint32 GetByte(const TArray<uint32>& Words, int32 Index)
	{
		return (Words[Index >> 2] >> ((Index & 3) * 8)) & 0xFFu;
	}

	void SetByte(TArray<uint32>& Words, int32 Index, uint32 Value)
	{
		Words[Index >> 2] |= (Value & 0xFFu) << ((Index & 3) * 8);
	}

	void ExpandBounds(FIntVector& Min, FIntVector& Max, bool& bAny, const FIntVector& Cell)
	{
		if (!bAny)
		{
			Min = Cell;
			Max = Cell;
			bAny = true;
			return;
		}
		Min = FIntVector(FMath::Min(Min.X, Cell.X), FMath::Min(Min.Y, Cell.Y), FMath::Min(Min.Z, Cell.Z));
		Max = FIntVector(FMath::Max(Max.X, Cell.X), FMath::Max(Max.Y, Cell.Y), FMath::Max(Max.Z, Cell.Z));
	}

	/** Обойти установленные биты плоскости жизни построчно, отдавая локальные
	 *  координаты и плоский индекс. Прыжками по битам, как распаковка
	 *  FGpuComputeStrategy - ветвление один раз на живую клетку, а не на ячейку. */
	template <typename FunctorType>
	void ForEachAliveBit(const TArray<uint32>& Bits, const FIntVector& Dim, FunctorType&& Functor)
	{
		for (int32 Z = 0; Z < Dim.Z; ++Z)
		{
			for (int32 Y = 0; Y < Dim.Y; ++Y)
			{
				const int32 RowStart = (Z * Dim.Y + Y) * Dim.X;
				int32 X = 0;
				while (X < Dim.X)
				{
					const int32 RowIndex = RowStart + X;
					const int32 BitInWord = RowIndex & 31;
					const int32 BitsAvailable = FMath::Min(32 - BitInWord, Dim.X - X);
					uint32 Word = Bits[RowIndex >> 5] >> BitInWord;
					if (BitsAvailable < 32)
					{
						Word &= (1u << BitsAvailable) - 1u;
					}
					while (Word != 0)
					{
						const int32 AliveX = X + (int32)FMath::CountTrailingZeros(Word);
						Functor(FIntVector(AliveX, Y, Z), RowStart + AliveX);
						Word &= Word - 1u;
					}
					X += BitsAvailable;
				}
			}
		}
	}

	/** Обойти ненулевые байты плоскости угасания (поклеточно: плоскость есть
	 *  только у правил Generations, и проход редкий - чтение назад и
	 *  раздвигание объёма). Живая клетка перебивает угасание. */
	template <typename FunctorType>
	void ForEachDecayingByte(const FGpuResidentSimulation* /*Unused*/, const TArray<uint32>& Decay, const TArray<uint32>& Bits,
		const FIntVector& Dim, FunctorType&& Functor)
	{
		int32 Index = 0;
		for (int32 Z = 0; Z < Dim.Z; ++Z)
		{
			for (int32 Y = 0; Y < Dim.Y; ++Y)
			{
				for (int32 X = 0; X < Dim.X; ++X, ++Index)
				{
					const uint32 State = GetByte(Decay, Index);
					if (State == 0 || ((Bits[Index >> 5] >> (Index & 31)) & 1u) != 0)
					{
						continue;
					}
					Functor(FIntVector(X, Y, Z), Index, (uint8)State);
				}
			}
		}
	}

	/** Выполнить команду на render thread и дождаться её. Тот же приём, что
	 *  у FGpuComputeStrategy: вызывающий ждёт на сыром FEvent (FRenderCommandFence
	 *  нельзя ждать с ThreadPool-воркера), опрос readback'ов - на render thread. */
	template <typename LambdaType>
	void RunOnRenderThreadAndWait(LambdaType&& Lambda)
	{
		FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);
		ENQUEUE_RENDER_COMMAND(GpuResidentSimulationCommand)(
			[Lambda = MoveTemp(Lambda), Done](FRHICommandListImmediate& RHICmdList) mutable
			{
				Lambda(RHICmdList);
				Done->Trigger();
			});
		Done->Wait();
		FPlatformProcess::ReturnSynchEventToPool(Done);
	}

	/** Дождаться readback'ов на render thread (после Execute и flush). */
	void WaitForReadbacks(FRHICommandListImmediate& RHICmdList, TArrayView<FRHIGPUBufferReadback* const> Readbacks)
	{
		// Без flush команды остаются в непроталкнутом списке, пока сам render
		// thread сидит в цикле ниже, и опрос не дождётся никогда (см. тот же
		// комментарий в GpuComputeStrategy.cpp).
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		for (FRHIGPUBufferReadback* Readback : Readbacks)
		{
			while (!Readback->IsReady())
			{
				FPlatformProcess::Sleep(0.0f);
			}
		}
	}
}

FGpuResidentSimulation::FGpuResidentSimulation()
	: GpuState(MakeShared<FGpuResidentPlanes, ESPMode::ThreadSafe>())
{
}

FGpuResidentSimulation::~FGpuResidentSimulation()
{
	// Пулированные буферы освобождаются на render thread - последняя ссылка
	// уезжает туда вместе с командой.
	ENQUEUE_RENDER_COMMAND(GpuResidentSimulationRelease)(
		[State = TSharedPtr<FGpuResidentPlanes, ESPMode::ThreadSafe>(GpuState)](FRHICommandListImmediate&) mutable
		{
			State.Reset();
		});
}

bool FGpuResidentSimulation::ChooseLayout(const FIntVector& BoundsMin, const FIntVector& BoundsMax, int32 MinHalo, int64 MaxVolumeCells,
	FIntVector& OutMinCell, FIntVector& OutDim, int64& OutCells, FString& OutError) const
{
	const FIntVector Size = BoundsMax - BoundsMin + FIntVector(1, 1, 1);
	// Запас - четверть самой длинной стороны, но не меньше MinHalo: так
	// раздвигание случается логарифмически редко при равномерном росте, а
	// маленькая структура не получает объём, который весь уйдёт на поля.
	int32 Halo = FMath::Max(MinHalo, Size.GetMax() / 4);

	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		OutMinCell = BoundsMin - FIntVector(Halo, Halo, Halo);
		OutDim = Size + FIntVector(2 * Halo, 2 * Halo, 2 * Halo);
		OutCells = int64(OutDim.X) * int64(OutDim.Y) * int64(OutDim.Z);
		if (OutCells > 0 && OutCells <= MaxVolumeCells && OutCells <= (int64)MAX_int32)
		{
			return true;
		}
		// Щедрый запас не влез - пробуем минимально необходимый.
		Halo = MinHalo;
	}

	OutError = FString::Printf(TEXT("объём %dx%dx%d = %lld клеток не влезает в лимит %lld (GpuVolumeCellLimit) даже с минимальным запасом %d"),
		OutDim.X, OutDim.Y, OutDim.Z, OutCells, MaxVolumeCells, MinHalo);
	return false;
}

bool FGpuResidentSimulation::Upload(const FCellGrid& Grid, const FCellularAutomatonRule& Rule, int32 MinHalo, int64 MaxVolumeCells, FString& OutError)
{
	const double StartSeconds = FPlatformTime::Seconds();
	bDecayPlane = Rule.HasDecayStates();

	TArray<FIntVector> AliveCells;
	Grid.GetAliveCells(AliveCells);
	TArray<FIntVector> DecayingCells;
	TArray<uint8> DecayingStates;
	if (bDecayPlane)
	{
		Grid.GetDecayingCells(DecayingCells, DecayingStates);
	}

	// Габарит - по живым И угасающим: угасающая клетка запрещает рождение в
	// своей точке, и потерять её значило бы разойтись с CPU.
	FIntVector BoundsMin = FIntVector::ZeroValue;
	FIntVector BoundsMax = FIntVector::ZeroValue;
	bool bAny = false;
	FGpuResidentStats NewStats;
	for (const FIntVector& Cell : AliveCells)
	{
		ExpandBounds(BoundsMin, BoundsMax, bAny, Cell);
	}
	NewStats.AliveCount = AliveCells.Num();
	NewStats.MinAlive = BoundsMin;
	NewStats.MaxAlive = BoundsMax;
	FIntVector DecayMin = FIntVector::ZeroValue;
	FIntVector DecayMax = FIntVector::ZeroValue;
	bool bAnyDecaying = false;
	for (const FIntVector& Cell : DecayingCells)
	{
		ExpandBounds(BoundsMin, BoundsMax, bAny, Cell);
		ExpandBounds(DecayMin, DecayMax, bAnyDecaying, Cell);
	}
	NewStats.DecayingCount = DecayingCells.Num();
	NewStats.MinDecaying = DecayMin;
	NewStats.MaxDecaying = DecayMax;

	FIntVector NewMinCell;
	FIntVector NewDim;
	int64 NewCells = 0;
	if (!ChooseLayout(BoundsMin, BoundsMax, FMath::Max(1, MinHalo), MaxVolumeCells, NewMinCell, NewDim, NewCells, OutError))
	{
		return false;
	}

	FPlanes Planes;
	Planes.Bits.SetNumZeroed(BitWords(NewCells));
	Planes.Ages.SetNumZeroed(ByteWords(NewCells));
	Planes.Decay.SetNumZeroed(bDecayPlane ? ByteWords(NewCells) : 1);

	for (const FIntVector& Cell : AliveCells)
	{
		const int32 Index = LinearIndex(Cell - NewMinCell, NewDim);
		Planes.Bits[Index >> 5] |= 1u << (Index & 31);
		SetByte(Planes.Ages, Index, Grid.GetAge(Cell));
	}
	for (int32 Index = 0; Index < DecayingCells.Num(); ++Index)
	{
		SetByte(Planes.Decay, LinearIndex(DecayingCells[Index] - NewMinCell, NewDim), DecayingStates[Index]);
	}

	MinCell = NewMinCell;
	VolumeDim = NewDim;
	VolumeCells = NewCells;
	Stats = NewStats;
	RegrowCount = 0;
	UploadPlanes(Planes);

	UE_LOG(LogTemp, Log, TEXT("GpuResident: залито %lld живых в объём %dx%dx%d (%lld клеток, %.1f МБ на GPU) за %.1f мс"),
		Stats.AliveCount, VolumeDim.X, VolumeDim.Y, VolumeDim.Z, VolumeCells,
		(double(Planes.Bits.Num()) * 2 + double(Planes.Ages.Num()) * 2 + (bDecayPlane ? double(Planes.Decay.Num()) * 2 : 0.0)) * 4.0 / (1024.0 * 1024.0),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	return true;
}

void FGpuResidentSimulation::UploadPlanes(const FPlanes& Planes)
{
	TSharedRef<FPlanes, ESPMode::ThreadSafe> Data = MakeShared<FPlanes, ESPMode::ThreadSafe>(Planes);
	RunOnRenderThreadAndWait([State = GpuState, Data, Min = MinCell, Dim = VolumeDim, bWithDecay = bDecayPlane](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		auto MakePair = [&GraphBuilder](const TCHAR* Name, const TArray<uint32>& Words, TRefCountPtr<FRDGPooledBuffer> (&Out)[2])
		{
			FRDGBufferRef First = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), Words.Num(), Words.GetData(), Words.Num() * sizeof(uint32));
			FRDGBufferRef Second = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Words.Num()), Name);
			// Вторая половина пары перезаписывается перед каждым проходом, но
			// вытащить из графа буфер, в который никто не писал, RDG не любит -
			// очищаем один раз.
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Second, PF_R32_UINT), 0u);
			Out[0] = GraphBuilder.ConvertToExternalBuffer(First);
			Out[1] = GraphBuilder.ConvertToExternalBuffer(Second);
		};

		MakePair(TEXT("CAResidentBits"), Data->Bits, State->Bits);
		MakePair(TEXT("CAResidentAges"), Data->Ages, State->Ages);
		MakePair(TEXT("CAResidentDecay"), Data->Decay, State->Decay);
		State->Current = 0;
		State->MinCell = Min;
		State->VolumeDim = Dim;
		State->bDecayPlane = bWithDecay;

		GraphBuilder.Execute();
	});
}

void FGpuResidentSimulation::ReadPlanes(FPlanes& OutPlanes) const
{
	TSharedRef<FPlanes, ESPMode::ThreadSafe> Data = MakeShared<FPlanes, ESPMode::ThreadSafe>();
	Data->Bits.SetNumZeroed(BitWords(VolumeCells));
	Data->Ages.SetNumZeroed(ByteWords(VolumeCells));
	Data->Decay.SetNumZeroed(bDecayPlane ? ByteWords(VolumeCells) : 1);

	RunOnRenderThreadAndWait([State = GpuState, Data, bWithDecay = bDecayPlane](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		const int32 Cur = State->Current;

		TArray<TSharedPtr<FRHIGPUBufferReadback>> Readbacks;
		auto Enqueue = [&](const TRefCountPtr<FRDGPooledBuffer>& Pooled)
		{
			TSharedPtr<FRHIGPUBufferReadback> Readback = MakeShared<FRHIGPUBufferReadback>(TEXT("CAResidentReadback"));
			FRDGBufferRef Buffer = GraphBuilder.RegisterExternalBuffer(Pooled);
			AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CAResidentReadback"), Buffer,
				[Readback, Buffer](FRHICommandListImmediate& InRHICmdList)
				{
					Readback->EnqueueCopy(InRHICmdList, Buffer->GetRHI(), 0);
				});
			Readbacks.Add(Readback);
		};
		Enqueue(State->Bits[Cur]);
		Enqueue(State->Ages[Cur]);
		if (bWithDecay)
		{
			Enqueue(State->Decay[Cur]);
		}
		GraphBuilder.Execute();

		TArray<FRHIGPUBufferReadback*> Raw;
		for (const TSharedPtr<FRHIGPUBufferReadback>& Readback : Readbacks)
		{
			Raw.Add(Readback.Get());
		}
		WaitForReadbacks(RHICmdList, Raw);

		auto Copy = [](FRHIGPUBufferReadback& Readback, TArray<uint32>& Out)
		{
			const void* Src = Readback.Lock(Out.Num() * sizeof(uint32));
			FMemory::Memcpy(Out.GetData(), Src, Out.Num() * sizeof(uint32));
			Readback.Unlock();
		};
		Copy(*Readbacks[0], Data->Bits);
		Copy(*Readbacks[1], Data->Ages);
		if (bWithDecay)
		{
			Copy(*Readbacks[2], Data->Decay);
		}
	});

	OutPlanes = MoveTemp(*Data);
}

void FGpuResidentSimulation::Download(FCellGrid& OutGrid) const
{
	if (!IsUploaded())
	{
		return;
	}
	const double StartSeconds = FPlatformTime::Seconds();

	FPlanes Planes;
	ReadPlanes(Planes);
	const double ReadSeconds = FPlatformTime::Seconds() - StartSeconds;

	ForEachAliveBit(Planes.Bits, VolumeDim, [&](const FIntVector& Local, int32 Index)
	{
		OutGrid.SetAliveWithAge(MinCell + Local, (uint8)GetByte(Planes.Ages, Index));
	});
	if (bDecayPlane)
	{
		ForEachDecayingByte(this, Planes.Decay, Planes.Bits, VolumeDim, [&](const FIntVector& Local, int32 /*Index*/, uint8 State)
		{
			OutGrid.SetDecayState(MinCell + Local, State);
		});
	}

	UE_LOG(LogTemp, Log, TEXT("GpuResident: прочитано назад %d живых за %.1f мс (readback %.1f, распаковка %.1f)"),
		OutGrid.Num(), (FPlatformTime::Seconds() - StartSeconds) * 1000.0, ReadSeconds * 1000.0,
		(FPlatformTime::Seconds() - StartSeconds - ReadSeconds) * 1000.0);
}

bool FGpuResidentSimulation::Regrow(int32 RequiredBand, int64 MaxVolumeCells, FString& OutError)
{
	const double StartSeconds = FPlatformTime::Seconds();

	// Новый габарит - по живым и угасающим, как при заливке; оба уже в сводке.
	FIntVector BoundsMin = FIntVector::ZeroValue;
	FIntVector BoundsMax = FIntVector::ZeroValue;
	bool bAny = false;
	if (Stats.AliveCount > 0)
	{
		ExpandBounds(BoundsMin, BoundsMax, bAny, Stats.MinAlive);
		ExpandBounds(BoundsMin, BoundsMax, bAny, Stats.MaxAlive);
	}
	if (Stats.DecayingCount > 0)
	{
		ExpandBounds(BoundsMin, BoundsMax, bAny, Stats.MinDecaying);
		ExpandBounds(BoundsMin, BoundsMax, bAny, Stats.MaxDecaying);
	}
	if (!bAny)
	{
		return true;
	}

	FIntVector NewMinCell;
	FIntVector NewDim;
	int64 NewCells = 0;
	if (!ChooseLayout(BoundsMin, BoundsMax, RequiredBand, MaxVolumeCells, NewMinCell, NewDim, NewCells, OutError))
	{
		return false;
	}

	const FIntVector OldDim = VolumeDim;
	const FIntVector OldOffsetInNew = MinCell - NewMinCell;
	const int32 NewBitWords = BitWords(NewCells);
	const int32 NewByteWords = ByteWords(NewCells);
	const bool bWithDecay = bDecayPlane;

	RunOnRenderThreadAndWait([State = GpuState, NewMinCell, NewDim, OldDim, OldOffsetInNew, NewBitWords, NewByteWords, bWithDecay](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		const int32 Cur = State->Current;

		// Новая пара на каждую плоскость: первая половина получает перекладку,
		// вторая просто чистая (её перезаписывает первый же шаг).
		auto MakePair = [&GraphBuilder](const TCHAR* Name, int32 Words, FRDGBufferRef (&Out)[2])
		{
			for (int32 Half = 0; Half < 2; ++Half)
			{
				Out[Half] = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Words), Name);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Out[Half], PF_R32_UINT), 0u);
			}
		};
		FRDGBufferRef NewBits[2];
		FRDGBufferRef NewAges[2];
		FRDGBufferRef NewDecay[2];
		MakePair(TEXT("CAResidentBits"), NewBitWords, NewBits);
		MakePair(TEXT("CAResidentAges"), NewByteWords, NewAges);
		MakePair(TEXT("CAResidentDecay"), bWithDecay ? NewByteWords : 1, NewDecay);

		FCellularAutomatonRelayoutCS::FParameters* Params = GraphBuilder.AllocParameters<FCellularAutomatonRelayoutCS::FParameters>();
		Params->NewDim = NewDim;
		Params->OldDim = OldDim;
		Params->OldOffsetInNew = OldOffsetInNew;
		Params->bDecayPlane = bWithDecay ? 1u : 0u;
		Params->InputBits = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(State->Bits[Cur]));
		Params->InputAges = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(State->Ages[Cur]));
		Params->InputDecay = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(State->Decay[Cur]));
		Params->OutputBits = GraphBuilder.CreateUAV(NewBits[0], PF_R32_UINT);
		Params->OutputAges = GraphBuilder.CreateUAV(NewAges[0], PF_R32_UINT);
		Params->OutputDecay = GraphBuilder.CreateUAV(NewDecay[0], PF_R32_UINT);

		TShaderMapRef<FCellularAutomatonRelayoutCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CAResidentRelayout"), Shader, Params,
			FComputeShaderUtils::GetGroupCount(NewDim, FIntVector(4, 4, 4)));

		for (int32 Half = 0; Half < 2; ++Half)
		{
			State->Bits[Half] = GraphBuilder.ConvertToExternalBuffer(NewBits[Half]);
			State->Ages[Half] = GraphBuilder.ConvertToExternalBuffer(NewAges[Half]);
			State->Decay[Half] = GraphBuilder.ConvertToExternalBuffer(NewDecay[Half]);
		}
		State->Current = 0;
		State->MinCell = NewMinCell;
		State->VolumeDim = NewDim;

		GraphBuilder.Execute();
	});

	MinCell = NewMinCell;
	VolumeDim = NewDim;
	VolumeCells = NewCells;
	++RegrowCount;

	UE_LOG(LogTemp, Log, TEXT("GpuResident: объём раздвинут %dx%dx%d -> %dx%dx%d (запас до края нужен %d, %.1f МБ на GPU) за %.1f мс"),
		OldDim.X, OldDim.Y, OldDim.Z, VolumeDim.X, VolumeDim.Y, VolumeDim.Z, RequiredBand,
		double(VolumeCells) * BytesPerCell(bDecayPlane) / (1024.0 * 1024.0),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	return true;
}

int32 FGpuResidentSimulation::Advance(const FCellularAutomatonRule& Rule, int32 NumSteps, int64 MaxVolumeCells, FString& OutError,
	const FExtraPass& ExtraPass)
{
	if (!IsUploaded())
	{
		OutError = TEXT("сессия не залита");
		return 0;
	}
	if (Rule.HasDecayStates() != bDecayPlane)
	{
		// Плоскость состояний заводится при заливке под режим правила; сменить
		// режим посреди сессии - значит перезалить, и это решает владелец.
		OutError = TEXT("правило сменило режим угасания (States) - сессию нужно перезалить");
		return 0;
	}

	const TArray<FIntVector>& NeighborOffsets = Rule.GetNeighborOffsets();
	if (NeighborOffsets.Num() > CellularAutomatonStepShader::MaxNeighborOffsets)
	{
		OutError = FString::Printf(TEXT("%d соседей больше потолка шейдера (%d)"), NeighborOffsets.Num(), CellularAutomatonStepShader::MaxNeighborOffsets);
		return 0;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const int32 Steps = FMath::Max(1, NumSteps);
	const int32 Extent = FMath::Max(1, Rule.GetNeighborExtent());
	const int32 Band = Extent * Steps;

	// Точность держится на одном условии: перед заходом ни одна живая клетка
	// не ближе Band к краю. Шейдер считает всё за краем мёртвым, и клетка,
	// которой положено родиться снаружи, иначе молча потерялась бы.
	bool bRegrown = false;
	if (Stats.AliveCount > 0)
	{
		const FIntVector MaxCell = MinCell + VolumeDim - FIntVector(1, 1, 1);
		const FIntVector Low = Stats.MinAlive - MinCell;
		const FIntVector High = MaxCell - Stats.MaxAlive;
		if (FMath::Min3(Low.X, Low.Y, Low.Z) < Band || FMath::Min3(High.X, High.Y, High.Z) < Band)
		{
			if (!Regrow(Band, MaxVolumeCells, OutError))
			{
				return 0;
			}
			bRegrown = true;
		}
	}

	TArray<FIntVector4> ShaderOffsets;
	ShaderOffsets.SetNumZeroed(CellularAutomatonStepShader::MaxNeighborOffsets);
	for (int32 Index = 0; Index < NeighborOffsets.Num(); ++Index)
	{
		ShaderOffsets[Index] = FIntVector4(NeighborOffsets[Index].X, NeighborOffsets[Index].Y, NeighborOffsets[Index].Z, 0);
	}
	uint32 BirthMask = 0;
	for (int32 Count : Rule.GetBirthCounts())
	{
		if (Count >= 0 && Count < 32)
		{
			BirthMask |= 1u << Count;
		}
	}
	uint32 SurvivalMask = 0;
	for (int32 Count : Rule.GetSurvivalCounts())
	{
		if (Count >= 0 && Count < 32)
		{
			SurvivalMask |= 1u << Count;
		}
	}

	const FIntVector Dim = VolumeDim;
	const int32 WordCount = BitWords(VolumeCells);
	const int32 NeighborCount = NeighborOffsets.Num();
	const uint32 MaxStates = (uint32)Rule.GetStates();
	const bool bWithDecay = bDecayPlane;
	TSharedRef<TArray<uint32>, ESPMode::ThreadSafe> StatsWords = MakeShared<TArray<uint32>, ESPMode::ThreadSafe>();
	StatsWords->SetNumZeroed(StatsWordCount);

	RunOnRenderThreadAndWait([State = GpuState, ShaderOffsets = MoveTemp(ShaderOffsets), BirthMask, SurvivalMask, Dim, WordCount,
		NeighborCount, MaxStates, bWithDecay, Steps, StatsWords, ExtraPass](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		const int32 Cur = State->Current;

		FRDGBufferRef BitsIn = GraphBuilder.RegisterExternalBuffer(State->Bits[Cur]);
		FRDGBufferRef BitsOut = GraphBuilder.RegisterExternalBuffer(State->Bits[1 - Cur]);
		FRDGBufferRef AgesIn = GraphBuilder.RegisterExternalBuffer(State->Ages[Cur]);
		FRDGBufferRef AgesOut = GraphBuilder.RegisterExternalBuffer(State->Ages[1 - Cur]);
		FRDGBufferRef DecayIn = GraphBuilder.RegisterExternalBuffer(State->Decay[Cur]);
		FRDGBufferRef DecayOut = GraphBuilder.RegisterExternalBuffer(State->Decay[1 - Cur]);

		TShaderMapRef<FCellularAutomatonStepCS> StepShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		for (int32 StepIndex = 0; StepIndex < Steps; ++StepIndex)
		{
			// Шейдер только выставляет биты (InterlockedOr) - выходы обязаны
			// быть чистыми перед каждым проходом.
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BitsOut, PF_R32_UINT), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(AgesOut, PF_R32_UINT), 0u);
			if (bWithDecay)
			{
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DecayOut, PF_R32_UINT), 0u);
			}

			FCellularAutomatonStepCS::FParameters* Params = GraphBuilder.AllocParameters<FCellularAutomatonStepCS::FParameters>();
			Params->VolumeDim = Dim;
			Params->NeighborOffsetCount = (uint32)NeighborCount;
			for (int32 Index = 0; Index < CellularAutomatonStepShader::MaxNeighborOffsets; ++Index)
			{
				Params->NeighborOffsets[Index] = ShaderOffsets[Index];
			}
			Params->BirthMask = BirthMask;
			Params->SurvivalMask = SurvivalMask;
			// В сессии возраст и угасание всегда ведёт шейдер: промежуточных
			// поколений на CPU нет вовсе.
			Params->bDecayActive = bWithDecay ? 1u : 0u;
			Params->bTrackAges = 1u;
			Params->bTrackDecayStates = bWithDecay ? 1u : 0u;
			Params->MaxStates = MaxStates;
			Params->InputBits = GraphBuilder.CreateSRV(BitsIn);
			// Битовая плоскость "угасает" нужна только вне режима состояний -
			// здесь не читается, заглушка на текущий вход (см. .usf).
			Params->InputDecayBits = GraphBuilder.CreateSRV(BitsIn);
			Params->InputAges = GraphBuilder.CreateSRV(AgesIn);
			Params->InputDecayStates = GraphBuilder.CreateSRV(DecayIn);
			Params->OutputBits = GraphBuilder.CreateUAV(BitsOut, PF_R32_UINT);
			Params->OutputAges = GraphBuilder.CreateUAV(AgesOut, PF_R32_UINT);
			Params->OutputDecayStates = GraphBuilder.CreateUAV(DecayOut, PF_R32_UINT);

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CAResidentStep(%d/%d)", StepIndex + 1, Steps),
				StepShader, Params, FComputeShaderUtils::GetGroupCount(Dim, FIntVector(4, 4, 4)));

			Swap(BitsIn, BitsOut);
			Swap(AgesIn, AgesOut);
			Swap(DecayIn, DecayOut);
		}

		// Сводка по итогу (он теперь в *In).
		FRDGBufferRef StatsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), StatsWordCount), TEXT("CAResidentStats"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(StatsBuffer, PF_R32_UINT), 0u);

		FCellularAutomatonStatsCS::FParameters* StatsParams = GraphBuilder.AllocParameters<FCellularAutomatonStatsCS::FParameters>();
		StatsParams->VolumeDim = Dim;
		StatsParams->WordCount = (uint32)WordCount;
		StatsParams->bDecayPlane = bWithDecay ? 1u : 0u;
		StatsParams->InputBits = GraphBuilder.CreateSRV(BitsIn);
		StatsParams->InputDecay = GraphBuilder.CreateSRV(DecayIn);
		StatsParams->OutputStats = GraphBuilder.CreateUAV(StatsBuffer, PF_R32_UINT);
		const int32 Groups = FMath::DivideAndRoundUp(WordCount, FCellularAutomatonStatsCS::GroupSize);
		// Сетка групп раскатывается во вторую размерность, если групп больше
		// 65535 - шейдер собирает индекс слова обратно (см. .usf).
		const FIntVector GroupCount(FMath::Min(Groups, 65535), FMath::DivideAndRoundUp(Groups, 65535), 1);
		TShaderMapRef<FCellularAutomatonStatsCS> StatsShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CAResidentStats"), StatsShader, StatsParams, GroupCount);

		if (ExtraPass)
		{
			ExtraPass(GraphBuilder, *State, BitsIn, AgesIn, DecayIn, GraphBuilder.CreateUAV(StatsBuffer, PF_R32_UINT));
		}

		TSharedPtr<FRHIGPUBufferReadback> Readback = MakeShared<FRHIGPUBufferReadback>(TEXT("CAResidentStatsReadback"));
		AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CAResidentStatsReadback"), StatsBuffer,
			[Readback, StatsBuffer](FRHICommandListImmediate& InRHICmdList)
			{
				Readback->EnqueueCopy(InRHICmdList, StatsBuffer->GetRHI(), 0);
			});

		GraphBuilder.Execute();
		// Нечётное число проходов - итог во второй половине пары.
		State->Current = (Steps & 1) ? 1 - Cur : Cur;

		FRHIGPUBufferReadback* Raw = Readback.Get();
		WaitForReadbacks(RHICmdList, MakeArrayView(&Raw, 1));
		const uint32* Src = (const uint32*)Readback->Lock(StatsWordCount * sizeof(uint32));
		FMemory::Memcpy(StatsWords->GetData(), Src, StatsWordCount * sizeof(uint32));
		Readback->Unlock();
	});

	const TArray<uint32>& Words = *StatsWords;
	Stats.AliveCount = Words[0];
	if (Stats.AliveCount > 0)
	{
		const FIntVector Last = Dim - FIntVector(1, 1, 1);
		Stats.MinAlive = MinCell + (Last - FIntVector((int32)Words[1], (int32)Words[2], (int32)Words[3]));
		Stats.MaxAlive = MinCell + FIntVector((int32)Words[4], (int32)Words[5], (int32)Words[6]);
	}
	Stats.DecayingCount = Words[8];
	if (Stats.DecayingCount > 0)
	{
		const FIntVector Last = Dim - FIntVector(1, 1, 1);
		Stats.MinDecaying = MinCell + (Last - FIntVector((int32)Words[9], (int32)Words[10], (int32)Words[11]));
		Stats.MaxDecaying = MinCell + FIntVector((int32)Words[12], (int32)Words[13], (int32)Words[14]);
	}
	Stats.ExtraCount = Words[ExtraStatsWord];
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Stats.ExtraAux[Index] = Words[ExtraStatsWord + 1 + Index];
	}

	UE_LOG(LogTemp, Log, TEXT("GpuResident: %d поколени(й) за %.2f мс%s - живых %lld, объём %dx%dx%d"),
		Steps, (FPlatformTime::Seconds() - StartSeconds) * 1000.0, bRegrown ? TEXT(" (с раздвиганием объёма)") : TEXT(""),
		Stats.AliveCount, VolumeDim.X, VolumeDim.Y, VolumeDim.Z);
	return Steps;
}
