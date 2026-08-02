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

	/** Размеры AABB живых клеток, расширенного на Halo клеток в каждую
	 *  сторону. Вынесено в функцию, потому что StepBatch() считает это в
	 *  цикле, подбирая максимальный размер пачки, который влезает в лимиты. */
	void ComputeHaloVolume(const FIntVector& MinAlive, const FIntVector& MaxAlive, int32 Halo,
		FIntVector& OutMinCell, FIntVector& OutVolumeDim, int64& OutVolumeCells)
	{
		OutMinCell = MinAlive - FIntVector(Halo, Halo, Halo);
		const FIntVector MaxCell = MaxAlive + FIntVector(Halo, Halo, Halo);
		OutVolumeDim = MaxCell - OutMinCell + FIntVector(1, 1, 1);
		OutVolumeCells = int64(OutVolumeDim.X) * int64(OutVolumeDim.Y) * int64(OutVolumeDim.Z);
	}
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
		// bDecayActive==0 (States==2, подавляющее большинство правил) -
		// InputDecayBits привязывается на тот же RDG-буфер, что InputBits
		// (см. Step() ниже) - шейдер его не разыменовывает под этой веткой,
		// так что лишнего аллоцирования не происходит.
		SHADER_PARAMETER(uint32, bDecayActive)
		// bTrackAges==0 - возрасты после шага считает CPU, как и раньше;
		// InputAges/OutputAges привязаны к заглушкам в одно слово (см.
		// StepBatch()). ==1 - шаг внутри пачки, промежуточных поколений на
		// CPU не будет, возраст ведёт шейдер.
		SHADER_PARAMETER(uint32, bTrackAges)
		// bTrackDecayStates==1 - шаг внутри пачки при States > 2: угасание
		// ведёт шейдер по байтовой плоскости состояний, потому что
		// CellDecay::AdvanceDecayStates() между поколениями пачки не
		// выполняется. ==0 - прежний путь с битовой плоскостью InputDecayBits
		// (см. .usf).
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

IMPLEMENT_GLOBAL_SHADER(FCellularAutomatonStepCS, "/Project/CellularAutomata/Private/CellularAutomatonStep.usf", "MainCS", SF_Compute);

void FGpuComputeStrategy::Step(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule) const
{
	// Одно поколение - частный случай пачки из одного шага: тогда ни гало
	// больше 1, ни плоскость возрастов не включаются, и путь получается
	// ровно прежним (см. StepBatch()).
	StepBatch(CurrentGrid, NextGrid, Rule, 1);
}

bool FGpuComputeStrategy::SupportsStepBatching(const FCellularAutomatonRule& Rule) const
{
	return true;
}

int32 FGpuComputeStrategy::StepBatch(const FCellGrid& CurrentGrid, FCellGrid& NextGrid, const FCellularAutomatonRule& Rule, int32 NumSteps) const
{
	const int32 RequestedSteps = FMath::Max(1, NumSteps);

	// Разбивка по фазам - тот же приём и тот же формат лога, что у
	// FCpuComputeStrategy::Step() (GetAliveCells/CandidateBuild/ParallelFor/
	// WriteBack). Здесь фазы свои: сбор+упаковка на CPU, круг через GPU
	// (заливка, диспатчи и блокирующее ожидание readback'а неразделимы с этой
	// стороны - всё внутри одной ENQUEUE_RENDER_COMMAND) и распаковка обратно
	// в сетку. Распаковка линейна по ОБЪЁМУ AABB, а не по числу живых клеток,
	// поэтому на разреженных структурах она вполне может доминировать - ровно
	// это и надо видеть в цифрах, а не предполагать. Ради этого же пачка и
	// существует: всё, кроме диспатчей, платится один раз на пачку.
	const double GetAliveStart = FPlatformTime::Seconds();
	TArray<FIntVector> AliveCells;
	CurrentGrid.GetAliveCells(AliveCells);
	const double GetAliveSeconds = FPlatformTime::Seconds() - GetAliveStart;

	if (AliveCells.Num() == 0)
	{
		// Пустая сетка - никакого GPU-буфера не строим, обнуляем прошлое
		// значение (см. GetLastComputeUploadBytes()'s doc-comment). Пустая
		// сетка остаётся пустой сколько угодно поколений подряд (родиться не
		// от чего), так что честно "продвигаем" всю запрошенную пачку - иначе
		// вызывающий цикл впустую прокрутил бы NumSteps итераций ни о чём.
		LastInputBufferBytes = 0;
		return RequestedSteps;
	}

	const double PackStart = FPlatformTime::Seconds();

	const TArray<FIntVector>& NeighborOffsets = Rule.GetNeighborOffsets();

	// Шейдерный массив офсетов имеет фиксированный размер, и переполнить его -
	// это не падение, а запись за границу TArray при заполнении ниже. Пары
	// (соседство, радиус), дающие больше офсетов, отсекаются на входе
	// (IsNeighborhoodRadiusSupported()), так что сюда попасть можно только
	// собрав FCellularAutomatonRule напрямую в обход оркестратора. Гвард
	// оставлен именно как гвард - и, по обычаю этого файла, уводит на CPU, а
	// не отказывает молча.
	//
	// Заодно он держит и второй потолок: маски правила 32-битные, а счётчик
	// соседей не может превысить число офсетов, так что при <= 26 офсетах
	// счётчики заведомо влезают в 32 бита (см. BirthMask/SurvivalMask ниже).
	if (NeighborOffsets.Num() > MaxShaderNeighborOffsets)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGpuComputeStrategy::StepBatch: %d соседей превышает потолок шейдерного массива (%d) - fallback на CPU"),
			NeighborOffsets.Num(), MaxShaderNeighborOffsets);
		LastInputBufferBytes = 0;
		FCpuComputeStrategy CpuFallback;
		CpuFallback.Step(CurrentGrid, NextGrid, Rule);
		return 1;
	}

	FIntVector MinAlive = AliveCells[0];
	FIntVector MaxAlive = AliveCells[0];
	for (const FIntVector& Cell : AliveCells)
	{
		MinAlive.X = FMath::Min(MinAlive.X, Cell.X);
		MinAlive.Y = FMath::Min(MinAlive.Y, Cell.Y);
		MinAlive.Z = FMath::Min(MinAlive.Z, Cell.Z);
		MaxAlive.X = FMath::Max(MaxAlive.X, Cell.X);
		MaxAlive.Y = FMath::Max(MaxAlive.Y, Cell.Y);
		MaxAlive.Z = FMath::Max(MaxAlive.Z, Cell.Z);
	}

	// Гало обязано равняться Radius * (число шагов в пачке). Компоненты
	// NeighborOffsets лежат в пределах [-Radius, Radius] (см.
	// CellularAutomatonRule.cpp::BuildNeighborOffsets), поэтому за поколение
	// структура вырастает максимум на Radius клеток в каждую сторону, а за K
	// поколений - на Radius*K, и с таким гало результат ТОЧЕН: всё, что могло
	// родиться, лежит внутри буфера. С гало меньше нужного пограничные клетки
	// молча терялись бы, и GPU разошёлся бы с CPU - поэтому подбирается не
	// гало под объём, а размер пачки под лимиты.
	//
	// Множитель Radius - причина, по которой радиус дороже всего обходится
	// именно пачке: при радиусе 2 тот же объём набирается вдвое меньшим
	// числом поколений, то есть StepsPerRender начинает урезаться раньше.
	// На стоимость одного диспатча радиус влияет только через число офсетов.
	const int32 NeighborRadius = FMath::Max(1, Rule.GetNeighborRadius());
	//
	// Generations (States > 2) пачке не мешает: угасание в этом режиме ведёт
	// сам шейдер по байтовой плоскости состояний (см. bTrackDecayStates ниже
	// и в .usf), а не CellDecay::AdvanceDecayStates() между поколениями.
	const bool bDecayActive = Rule.HasDecayStates();
	int32 EffectiveSteps = RequestedSteps;

	FIntVector MinCell;
	FIntVector VolumeDim;
	int64 VolumeCells = 0;
	ComputeHaloVolume(MinAlive, MaxAlive, NeighborRadius * EffectiveSteps, MinCell, VolumeDim, VolumeCells);

	// Пачка не влезает - урезаем её (а не отбрасываем): меньше шагов -> меньше
	// гало -> меньше объём, так что цикл сходится монотонно. Даже пачка из 3
	// шагов вместо 10 экономит две трети кругов через GPU.
	const int64 BatchLimit = BatchVolumeCellLimit(bDecayActive);
	while (EffectiveSteps > 1 && (VolumeCells <= 0 || VolumeCells > MaxVolumeCells || VolumeCells > BatchLimit))
	{
		--EffectiveSteps;
		ComputeHaloVolume(MinAlive, MaxAlive, NeighborRadius * EffectiveSteps, MinCell, VolumeDim, VolumeCells);
	}

	// Защита от OOM: две далёкие друг от друга живые клетки в разреженной
	// сетке могли бы раздуть AABB до неподъёмного объёма - в этом случае
	// откатываемся на CPU вместо аллокации гигантского битового буфера.
	// Лимит настраивается через AAutomataOrchestrator::GpuVolumeCellLimit
	// (передан в конструктор), а не зашит константой.
	if (VolumeCells <= 0 || VolumeCells > MaxVolumeCells)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGpuComputeStrategy::StepBatch: ограничивающий объём (%lld клеток) превышает лимит GPU-буфера - fallback на CPU"), VolumeCells);
		// Фолбэк на CPU не строит GPU-буфер в этот раз - та же причина, что
		// и на пустой сетке выше.
		LastInputBufferBytes = 0;
		FCpuComputeStrategy CpuFallback;
		CpuFallback.Step(CurrentGrid, NextGrid, Rule);
		return 1;
	}

	// Возрасты ведёт GPU ровно тогда, когда идёт настоящая пачка - при одном
	// шаге их по-прежнему считает CellAging::ComputeAges() на CPU, и лишней
	// плоскости по байту на клетку не создаётся вовсе.
	const bool bTrackAges = EffectiveSteps > 1;

	// Угасание ведёт шейдер ровно тогда же, когда и возрасты, и по той же
	// причине: внутри пачки промежуточных поколений на CPU не существует,
	// а CellDecay::AdvanceDecayStates() умеет продвигать состояние только с
	// одного поколения на соседнее. При одиночном шаге остаётся прежний путь -
	// битовая плоскость "угасает" на вход и CPU-проход после.
	const bool bTrackDecayStates = bDecayActive && EffectiveSteps > 1;

	// ПОТОЛОК ВСЕГО GPU-ПУТИ - MAX_int32 клеток объёма, и он жёсткий: индексы
	// в упаковке, в распаковке и в шейдере - 32-битные, так что объём больше
	// 2 147 483 647 переполнил бы их молча. Раньше здесь стояло голое
	// приведение (int32)VolumeCells, и гвард выше пропускал всё, что меньше
	// GpuVolumeCellLimit - то есть при лимите в миллиарды объём между 2.1 млрд
	// и лимитом проходил проверку, а размер буфера считался по обрезанному
	// числу. Это не падение и не откат, это запись за границы буфера.
	//
	// Здесь честный отказ вместо этого: объём сверх потолка уводится на CPU
	// тем же путём, что и превышение GpuVolumeCellLimit. Поднять потолок
	// по-настоящему - значит перевести всю индексацию на int64, это отдельная
	// работа, а не правка одной строки.
	if (VolumeCells > (int64)MAX_int32)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGpuComputeStrategy::StepBatch: объём %lld превышает 32-битный потолок индексации (%d) - fallback на CPU. GpuVolumeCellLimit выше этого значения смысла не имеет."),
			VolumeCells, MAX_int32);
		LastInputBufferBytes = 0;
		FCpuComputeStrategy CpuFallback;
		CpuFallback.Step(CurrentGrid, NextGrid, Rule);
		return 1;
	}

	const int32 VolumeCellsI32 = (int32)VolumeCells;
	const int32 WordCount = FMath::DivideAndRoundUp(VolumeCellsI32, 32);
	const int32 BytePlaneWordCount = FMath::DivideAndRoundUp(VolumeCellsI32, 4);
	const int32 AgeWordCount = bTrackAges ? BytePlaneWordCount : 1;
	const int32 DecayStateWordCount = bTrackDecayStates ? BytePlaneWordCount : 1;

	TArray<uint32> InputWords;
	InputWords.SetNumZeroed(WordCount);
	TArray<uint32> InputAgeWords;
	if (bTrackAges)
	{
		InputAgeWords.SetNumZeroed(AgeWordCount);
	}

	for (const FIntVector& Cell : AliveCells)
	{
		const FIntVector Local = Cell - MinCell;
		const int32 Index = (Local.Z * VolumeDim.Y + Local.Y) * VolumeDim.X + Local.X;
		InputWords[Index >> 5] |= (1u << (Index & 31));

		if (bTrackAges)
		{
			// Возраст на входе в пачку - чтобы клетка, пережившая её целиком,
			// продолжила счёт с прежнего значения, а не начала с нуля (ровно
			// то, что делает CellAging::ComputeAges() между поколениями).
			const uint32 Age = CurrentGrid.GetAge(Cell);
			InputAgeWords[Index >> 2] |= (Age << ((Index & 3) * 8));
		}
	}

	// При Rule.HasDecayStates() (States > 2) собираем угасающие клетки той же
	// AABB/индексацией, что InputWords, БЕЗ расширения границ под них:
	// угасающая клетка вдали от всего живого всё равно не может родиться
	// (birth возможен только рядом с чем-то живым). Форма зависит от режима:
	// под пачкой - байтовая плоскость состояний (шейдеру нужна конкретная
	// стадия, чтобы её продвигать), иначе - прежняя битовая плоскость "просто
	// угасает" (стадию в этом режиме продвигает CPU после шага, и её
	// собственное продвижение от этого AABB не зависит вовсе). При States == 2
	// не выполняется ни то, ни другое - ни GetDecayingCells(), ни аллокация.
	TArray<uint32> InputDecayWords;
	TArray<uint32> InputDecayStateWords;
	if (bDecayActive)
	{
		if (bTrackDecayStates)
		{
			InputDecayStateWords.SetNumZeroed(DecayStateWordCount);
		}
		else
		{
			InputDecayWords.SetNumZeroed(WordCount);
		}

		TArray<FIntVector> DecayingCells;
		TArray<uint8> DecayingStates;
		CurrentGrid.GetDecayingCells(DecayingCells, DecayingStates);
		const FIntVector MaxCell = MinCell + VolumeDim - FIntVector(1, 1, 1);
		for (int32 CellIndex = 0; CellIndex < DecayingCells.Num(); ++CellIndex)
		{
			const FIntVector& Cell = DecayingCells[CellIndex];
			if (Cell.X < MinCell.X || Cell.X > MaxCell.X ||
				Cell.Y < MinCell.Y || Cell.Y > MaxCell.Y ||
				Cell.Z < MinCell.Z || Cell.Z > MaxCell.Z)
			{
				continue;
			}

			const FIntVector Local = Cell - MinCell;
			const int32 Index = (Local.Z * VolumeDim.Y + Local.Y) * VolumeDim.X + Local.X;
			if (bTrackDecayStates)
			{
				InputDecayStateWords[Index >> 2] |= (uint32(DecayingStates[CellIndex]) << ((Index & 3) * 8));
			}
			else
			{
				InputDecayWords[Index >> 5] |= (1u << (Index & 31));
			}
		}
	}

	// См. GetLastComputeUploadBytes() - размер входных буферов, которые ниже
	// реально уходят в CreateStructuredBuffer(); учитывает второй битовый и
	// возрастной буферы, только если они реально были загружены. Для пачки
	// это по-прежнему ОДНА заливка, а не по заливке на поколение - в этом вся
	// суть StepBatch().
	LastInputBufferBytes = int64(WordCount) * sizeof(uint32) * ((bDecayActive && !bTrackDecayStates) ? 2 : 1)
		+ (bTrackAges ? int64(AgeWordCount) * sizeof(uint32) : 0)
		+ (bTrackDecayStates ? int64(DecayStateWordCount) * sizeof(uint32) : 0);

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

	const double PackSeconds = FPlatformTime::Seconds() - PackStart;
	const double GpuRoundTripStart = FPlatformTime::Seconds();

	// Диспатчи, readback-опрос и Lock/Unlock идут ЦЕЛИКОМ на Render Thread -
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
	TSharedRef<TArray<uint32>> OutputAgeWords = MakeShared<TArray<uint32>>();
	OutputAgeWords->SetNumZeroed(bTrackAges ? AgeWordCount : 0);
	TSharedRef<TArray<uint32>> OutputDecayStateWords = MakeShared<TArray<uint32>>();
	OutputDecayStateWords->SetNumZeroed(bTrackDecayStates ? DecayStateWordCount : 0);

	const uint32 MaxStates = (uint32)Rule.GetStates();

	FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(false);
	ENQUEUE_RENDER_COMMAND(CellularAutomatonStepCommand)(
		[InputWords = MoveTemp(InputWords), InputDecayWords = MoveTemp(InputDecayWords), InputAgeWords = MoveTemp(InputAgeWords),
		 InputDecayStateWords = MoveTemp(InputDecayStateWords), ShaderOffsets = MoveTemp(ShaderOffsets),
		 WordCount, AgeWordCount, DecayStateWordCount, VolumeDim, NeighborOffsetCount, BirthMask, SurvivalMask, MaxStates,
		 bDecayActive, bTrackAges, bTrackDecayStates, EffectiveSteps, OutputWords, OutputAgeWords, OutputDecayStateWords,
		 CompletionEvent](FRHICommandListImmediate& RHICmdList) mutable
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			// Ping-pong: состояние живёт в GPU-памяти всю пачку, между
			// поколениями буферы просто меняются ролями - именно это и убирает
			// круг CPU->GPU->CPU на каждое поколение. Барьеры между проходами
			// расставляет сам RDG (SRV одного прохода - UAV предыдущего).
			FRDGBufferRef BitsIn = CreateStructuredBuffer(GraphBuilder, TEXT("CAInputBits"),
				sizeof(uint32), WordCount, InputWords.GetData(), WordCount * sizeof(uint32));
			FRDGBufferRef BitsOut = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), WordCount), TEXT("CAOutputBits"));

			// Битовая плоскость "угасает" нужна ТОЛЬКО вне пачки: под пачкой ту
			// же роль играет байтовая плоскость состояний, и InputDecayWords
			// намеренно остаётся пустым - создавать из него буфер на WordCount
			// слов значило бы прочитать за пределы пустого массива.
			FRDGBufferRef InputDecayBuffer = (bDecayActive && !bTrackDecayStates)
				? CreateStructuredBuffer(GraphBuilder, TEXT("CAInputDecayBits"),
					sizeof(uint32), WordCount, InputDecayWords.GetData(), WordCount * sizeof(uint32))
				: nullptr;

			// Возрастные буферы создаются, только если возраст реально ведётся
			// (то есть только под пачкой) - иначе SRV-слоты ниже получают
			// текущий BitsIn как безобидную заглушку, а вот UAV должен быть
			// отдельным буфером: привязывать BitsOut вторым UAV в том же
			// проходе значило бы дважды объявить один ресурс на запись.
			FRDGBufferRef AgesIn = bTrackAges
				? CreateStructuredBuffer(GraphBuilder, TEXT("CAInputAges"),
					sizeof(uint32), AgeWordCount, InputAgeWords.GetData(), AgeWordCount * sizeof(uint32))
				: nullptr;
			FRDGBufferRef AgesOut = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), bTrackAges ? AgeWordCount : 1), TEXT("CAOutputAges"));

			// Плоскость угасающих состояний - устроена точно как возрастная
			// (создаётся только когда реально ведётся, ping-pong'ается тем же
			// свапом, неиспользуемый SRV-слот берёт текущий BitsIn).
			FRDGBufferRef DecayStatesIn = bTrackDecayStates
				? CreateStructuredBuffer(GraphBuilder, TEXT("CAInputDecayStates"),
					sizeof(uint32), DecayStateWordCount, InputDecayStateWords.GetData(), DecayStateWordCount * sizeof(uint32))
				: nullptr;
			FRDGBufferRef DecayStatesOut = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), bTrackDecayStates ? DecayStateWordCount : 1), TEXT("CAOutputDecayStates"));

			for (int32 StepIndex = 0; StepIndex < EffectiveSteps; ++StepIndex)
			{
				// Шейдер только выставляет биты (InterlockedOr), нулей не
				// пишет - выходной буфер обязан быть чистым перед каждым
				// проходом, иначе в него подмешался бы позапрошлый результат.
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BitsOut, PF_R32_UINT), 0u);
				if (bTrackAges)
				{
					AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(AgesOut, PF_R32_UINT), 0u);
				}
				if (bTrackDecayStates)
				{
					AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DecayStatesOut, PF_R32_UINT), 0u);
				}

				FCellularAutomatonStepCS::FParameters* Params = GraphBuilder.AllocParameters<FCellularAutomatonStepCS::FParameters>();
				Params->VolumeDim = VolumeDim;
				Params->NeighborOffsetCount = (uint32)NeighborOffsetCount;
				for (int32 Index = 0; Index < MaxShaderNeighborOffsets; ++Index)
				{
					Params->NeighborOffsets[Index] = ShaderOffsets[Index];
				}
				Params->BirthMask = BirthMask;
				Params->SurvivalMask = SurvivalMask;
				Params->bDecayActive = bDecayActive ? 1u : 0u;
				Params->bTrackAges = bTrackAges ? 1u : 0u;
				Params->bTrackDecayStates = bTrackDecayStates ? 1u : 0u;
				Params->MaxStates = MaxStates;
				// Неиспользуемые SRV-слоты привязываются к ТЕКУЩЕМУ BitsIn, а
				// не к запомненному один раз до цикла: BitsIn/BitsOut меняются
				// ролями каждый проход, и заглушка, взятая до цикла, со второго
				// прохода указывала бы на буфер, который в этом же проходе
				// является целью записи - один ресурс сразу как SRV и как UAV.
				// Шейдер эти слоты под своими гейтами не читает, но RDG
				// валидирует привязки независимо от того, читает их шейдер или
				// нет.
				Params->InputBits = GraphBuilder.CreateSRV(BitsIn);
				Params->InputDecayBits = GraphBuilder.CreateSRV(InputDecayBuffer ? InputDecayBuffer : BitsIn);
				Params->InputAges = GraphBuilder.CreateSRV(bTrackAges ? AgesIn : BitsIn);
				Params->InputDecayStates = GraphBuilder.CreateSRV(bTrackDecayStates ? DecayStatesIn : BitsIn);
				Params->OutputBits = GraphBuilder.CreateUAV(BitsOut, PF_R32_UINT);
				Params->OutputAges = GraphBuilder.CreateUAV(AgesOut, PF_R32_UINT);
				Params->OutputDecayStates = GraphBuilder.CreateUAV(DecayStatesOut, PF_R32_UINT);

				TShaderMapRef<FCellularAutomatonStepCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CellularAutomatonStep(%d/%d)", StepIndex + 1, EffectiveSteps),
					ComputeShader, Params, FComputeShaderUtils::GetGroupCount(VolumeDim, FIntVector(4, 4, 4)));

				// После обмена результат этого прохода лежит в BitsIn - то
				// есть по выходу из цикла итог пачки всегда в BitsIn/AgesIn.
				Swap(BitsIn, BitsOut);
				if (bTrackAges)
				{
					Swap(AgesIn, AgesOut);
				}
				if (bTrackDecayStates)
				{
					Swap(DecayStatesIn, DecayStatesOut);
				}
			}

			TSharedRef<FRHIGPUBufferReadback> Readback = MakeShared<FRHIGPUBufferReadback>(TEXT("CellularAutomatonStepReadback"));
			AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CellularAutomatonStepReadback"), BitsIn,
				[Readback, BitsIn](FRHICommandListImmediate& RHICmdListLocal)
				{
					Readback->EnqueueCopy(RHICmdListLocal, BitsIn->GetRHI(), 0);
				});

			// Возрасты забираются вторым readback'ом - тоже ОДИН раз на всю
			// пачку, не на поколение.
			TSharedPtr<FRHIGPUBufferReadback> AgeReadback;
			if (bTrackAges)
			{
				AgeReadback = MakeShared<FRHIGPUBufferReadback>(TEXT("CellularAutomatonAgeReadback"));
				AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CellularAutomatonAgeReadback"), AgesIn,
					[AgeReadback, AgesIn](FRHICommandListImmediate& RHICmdListLocal)
					{
						AgeReadback->EnqueueCopy(RHICmdListLocal, AgesIn->GetRHI(), 0);
					});
			}

			TSharedPtr<FRHIGPUBufferReadback> DecayStateReadback;
			if (bTrackDecayStates)
			{
				DecayStateReadback = MakeShared<FRHIGPUBufferReadback>(TEXT("CellularAutomatonDecayStateReadback"));
				AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CellularAutomatonDecayStateReadback"), DecayStatesIn,
					[DecayStateReadback, DecayStatesIn](FRHICommandListImmediate& RHICmdListLocal)
					{
						DecayStateReadback->EnqueueCopy(RHICmdListLocal, DecayStatesIn->GetRHI(), 0);
					});
			}

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
			// комментарий выше про причину переноса всего блока сюда). Ждём оба
			// readback'а: они enqueue'ятся в один граф, но готовы могут стать
			// не одновременно.
			while (!Readback->IsReady()
				|| (AgeReadback.IsValid() && !AgeReadback->IsReady())
				|| (DecayStateReadback.IsValid() && !DecayStateReadback->IsReady()))
			{
				FPlatformProcess::Sleep(0.0f);
			}

			const uint32* ResultData = (const uint32*)Readback->Lock(WordCount * sizeof(uint32));
			FMemory::Memcpy(OutputWords->GetData(), ResultData, WordCount * sizeof(uint32));
			Readback->Unlock();

			if (AgeReadback.IsValid())
			{
				const uint32* AgeData = (const uint32*)AgeReadback->Lock(AgeWordCount * sizeof(uint32));
				FMemory::Memcpy(OutputAgeWords->GetData(), AgeData, AgeWordCount * sizeof(uint32));
				AgeReadback->Unlock();
			}

			if (DecayStateReadback.IsValid())
			{
				const uint32* DecayStateData = (const uint32*)DecayStateReadback->Lock(DecayStateWordCount * sizeof(uint32));
				FMemory::Memcpy(OutputDecayStateWords->GetData(), DecayStateData, DecayStateWordCount * sizeof(uint32));
				DecayStateReadback->Unlock();
			}

			CompletionEvent->Trigger();
		});

	CompletionEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);

	const double GpuRoundTripSeconds = FPlatformTime::Seconds() - GpuRoundTripStart;
	const double UnpackStart = FPlatformTime::Seconds();

	// Обход ПО СЛОВАМ с прыжками по установленным битам, а не по ячейкам.
	//
	// Прежний поклеточный цикл спрашивал бит жизни у каждой из десятков
	// миллионов ячеек объёма. Тело такой итерации - загрузка слова, сдвиг,
	// маска и переход, полтора-два такта; замер же давал ~1.9 нс на ячейку
	// объёма, то есть шесть-семь тактов. Разница - штраф за промах
	// предсказателя переходов: бит жизни при заполнении около 13% и
	// фрактальной структуре не предсказывается в принципе, а промах стоит
	// порядка полутора десятков тактов.
	//
	// CountTrailingZeros по слову превращает "спросить у каждой ячейки" в
	// "перейти к следующей живой": ветвление случается один раз на живую
	// клетку, а не один раз на ячейку объёма, и на мёртвых ячейках
	// предсказателю нечего угадывать - их не перебирают поштучно.
	//
	// Обход построчный (а не сплошной по всему буферу), чтобы координата X
	// бралась из позиции бита внутри строки: так в горячем цикле не остаётся
	// ни одного деления для обратного разбора плоского индекса.
	//
	// Здесь же пробовался чанк-выровненный обход - чтобы кеш чанка в
	// FDenseCellGrid попадал почти всегда, а не примерно в половине случаев.
	// Он отработал (проверено диагностикой в логе), но не дал ничего: ~15.5
	// нс на живую клетку и до, и после. Значит остаток стоимости - не поиск
	// чанка, и усложнение обхода было выброшено. Заодно тот обход с этим
	// несовместим: слова идут вдоль X и границ чанков не уважают.
	for (int32 LocalZ = 0; LocalZ < VolumeDim.Z; ++LocalZ)
	{
		const int32 CellZ = MinCell.Z + LocalZ;
		for (int32 LocalY = 0; LocalY < VolumeDim.Y; ++LocalY)
		{
			const int32 CellY = MinCell.Y + LocalY;
			const int32 RowStart = (LocalZ * VolumeDim.Y + LocalY) * VolumeDim.X;

			int32 LocalX = 0;
			while (LocalX < VolumeDim.X)
			{
				const int32 RowIndex = RowStart + LocalX;
				const int32 BitInWord = RowIndex & 31;

				// Сколько бит этого слова ещё принадлежат текущей строке:
				// слово может кончиться раньше строки, а может и выйти за её
				// конец - VolumeDim.X не кратен 32.
				const int32 BitsAvailable = FMath::Min(32 - BitInWord, VolumeDim.X - LocalX);

				uint32 Word = (*OutputWords)[RowIndex >> 5] >> BitInWord;
				if (BitsAvailable < 32)
				{
					// Сдвиг на 32 - неопределённое поведение, поэтому маска
					// накладывается только когда бит действительно меньше 32.
					Word &= (1u << BitsAvailable) - 1u;
				}

				while (Word != 0)
				{
					const int32 AliveLocalX = LocalX + (int32)FMath::CountTrailingZeros(Word);
					const int32 Index = RowStart + AliveLocalX;
					const FIntVector Cell(MinCell.X + AliveLocalX, CellY, CellZ);

					if (bTrackAges)
					{
						// Оживление и возраст - ОДНИМ вызовом: раздельные
						// SetAlive()+SetAge() искали место для одной и той же
						// клетки дважды, причём второй раз без кеша чанка
						// (см. FCellGrid::SetAliveWithAge()). На 19.5 млн
						// клеток это стоило ~200 мс из 318.
						const uint8 Age = (uint8)(((*OutputAgeWords)[Index >> 2] >> ((Index & 3) * 8)) & 0xFFu);
						NextGrid.SetAliveWithAge(Cell, Age);
					}
					else
					{
						// Возрасты посчитает CellAging::ComputeAges() после
						// шага - плоскость возрастов с GPU даже не приезжала.
						NextGrid.SetAlive(Cell, true);
					}

					// Сбросить младший установленный бит и перейти к следующему.
					Word &= Word - 1u;
				}

				LocalX += BitsAvailable;
			}
		}
	}

	// Угасающие клетки - отдельным проходом. Объединить его с проходом выше
	// нельзя: угасающая клетка по определению НЕ живая, её бит в основной
	// плоскости нулевой, и прыжки по установленным битам её не увидят.
	// Проход поклеточный и без хитростей - он выполняется только для правил
	// Generations (States > 2), а оптимизировался бинарный случай, где эта
	// плоскость вообще не запрашивается.
	if (bTrackDecayStates)
	{
		int32 Index = 0;
		for (int32 LocalZ = 0; LocalZ < VolumeDim.Z; ++LocalZ)
		{
			const int32 CellZ = MinCell.Z + LocalZ;
			for (int32 LocalY = 0; LocalY < VolumeDim.Y; ++LocalY)
			{
				const int32 CellY = MinCell.Y + LocalY;
				for (int32 LocalX = 0; LocalX < VolumeDim.X; ++LocalX, ++Index)
				{
					const uint8 DecayState = (uint8)(((*OutputDecayStateWords)[Index >> 2] >> ((Index & 3) * 8)) & 0xFFu);
					if (DecayState == 0)
					{
						continue;
					}

					// Живая клетка перебивает угасание - ровно как в прежнем
					// едином цикле, где ветка SetDecayState() стояла в else к
					// проверке на жизнь.
					if ((((*OutputWords)[Index >> 5] >> (Index & 31)) & 1u) != 0)
					{
						continue;
					}

					NextGrid.SetDecayState(FIntVector(MinCell.X + LocalX, CellY, CellZ), DecayState);
				}
			}
		}
	}

	const double UnpackSeconds = FPlatformTime::Seconds() - UnpackStart;
	const double TotalSeconds = FPlatformTime::Seconds() - GetAliveStart;

	// Гало и число соседей в строке - не украшение: именно по ним видно, во
	// что обошёлся радиус (гало = Radius*поколений, объём растёт по кубу от
	// него), и именно расхождение здесь выдаёт неверно посчитанное гало.
	UE_LOG(LogTemp, Log, TEXT("GpuStep: живых %d -> объём %dx%dx%d = %lld клеток (радиус %d, соседей %d, гало %d), поколений за круг: %d из %d (шаг: %.2f мс [GetAliveCells: %.2f, Pack: %.2f, GpuRoundTrip: %.2f, Unpack: %.2f])"),
		AliveCells.Num(), VolumeDim.X, VolumeDim.Y, VolumeDim.Z, VolumeCells,
		NeighborRadius, NeighborOffsets.Num(), NeighborRadius * EffectiveSteps,
		EffectiveSteps, RequestedSteps, TotalSeconds * 1000.0,
		GetAliveSeconds * 1000.0, PackSeconds * 1000.0, GpuRoundTripSeconds * 1000.0, UnpackSeconds * 1000.0);

	return EffectiveSteps;
}
