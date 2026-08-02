#include "Automata/Generation/StateGenerators.h"

#include "Automata/Simulation/CellularAutomatonRule.h"
#include "Algo/Sort.h"
#include "Algo/Unique.h"
#include "Math/RandomStream.h"

namespace
{
	/** Деление с округлением ВНИЗ. Обычное целочисленное деление в C++
	 *  округляет к нулю, поэтому при отрицательных координатах (а они здесь
	 *  норма - область центрирована в нуле) шахматный узор ломался бы ровно на
	 *  нуле, то есть в центре кадра. Та же ловушка, что описана в
	 *  DenseCellGrid.cpp про чанк-координаты. */
	FORCEINLINE int32 FloorDivInt(int32 Value, int32 Divisor)
	{
		return (Value >= 0) ? (Value / Divisor) : -(((-Value) + Divisor - 1) / Divisor);
	}

	/** Неотрицательный остаток: -3 % 8 в C++ это -3, а решётке нужен 5. */
	FORCEINLINE int32 WrapMod(int32 Value, int32 Period)
	{
		const int32 Rest = Value % Period;
		return (Rest < 0) ? (Rest + Period) : Rest;
	}

	/** Сколько узлов решётки с шагом Period укладывается по одну сторону от
	 *  нуля: узлы идут от -N до +N включительно, то есть их 2N+1. */
	FORCEINLINE int32 NodeHalfCount(int32 Extent, int32 Period)
	{
		return Extent / FMath::Max(Period, 1);
	}

	/** Число клеток по оси: область [-Extent, +Extent] - длина нечётная,
	 *  центральная клетка ровно в нуле. */
	FORCEINLINE int64 AxisDim(int32 Extent)
	{
		return 2LL * Extent + 1;
	}

	FORCEINLINE int64 BoxVolume(const FIntVector& Extent)
	{
		return AxisDim(Extent.X) * AxisDim(Extent.Y) * AxisDim(Extent.Z);
	}

	/** Целочисленный корень: наибольшее X, при котором X*X <= Value. Через
	 *  double с явной поправкой, потому что FMath::Sqrt() на больших
	 *  аргументах может ошибиться на единицу в любую сторону, а от этого
	 *  зависит, окажется оценка выше факта или ниже. */
	int32 IntSqrt(int64 Value)
	{
		if (Value <= 0)
		{
			return 0;
		}

		int32 Root = static_cast<int32>(FMath::Sqrt(static_cast<double>(Value)));
		while (static_cast<int64>(Root + 1) * (Root + 1) <= Value)
		{
			++Root;
		}
		while (Root > 0 && static_cast<int64>(Root) * Root > Value)
		{
			--Root;
		}
		return Root;
	}

	/** ТОЧНОЕ число клеток решётки в шаре радиуса Radius, за O(Radius^2) - без
	 *  перебора всего куба.
	 *
	 *  Непрерывный объём 4/3*pi*R^3 здесь не годится: он не является верхней
	 *  границей для числа целых точек, а на полой сфере ошибки двух таких
	 *  приближений складываются и оценка уходит НИЖЕ факта - то есть перестаёт
	 *  защищать бюджет, ради которого существует (поймано автотестом
	 *  Generation.Determinism: 708 против 744). */
	int64 CountLatticePointsWithinSq(int64 RadiusSq)
	{
		if (RadiusSq < 0)
		{
			return 0;
		}

		const int32 Radius = IntSqrt(RadiusSq);
		int64 Total = 0;

		for (int32 z = -Radius; z <= Radius; ++z)
		{
			const int64 RemainderZ = RadiusSq - static_cast<int64>(z) * z;
			if (RemainderZ < 0)
			{
				continue;
			}

			const int32 MaxY = IntSqrt(RemainderZ);
			for (int32 y = -MaxY; y <= MaxY; ++y)
			{
				const int64 Remainder = RemainderZ - static_cast<int64>(y) * y;
				// По оси X помещается 2*floor(sqrt(Remainder)) + 1 клеток.
				Total += 2LL * IntSqrt(Remainder) + 1;
			}
		}

		return Total;
	}

	/** Точное число клеток решётки в шаре радиуса Radius. */
	FORCEINLINE int64 CountBallLatticePoints(int32 Radius)
	{
		return (Radius < 0) ? 0 : CountLatticePointsWithinSq(static_cast<int64>(Radius) * Radius);
	}

	/** Приёмник клеток с потолком. Потолок проверяется по факту, а не только по
	 *  оценке: у шумовых генераторов оценка приблизительная, а обрывать работу
	 *  надо до того, как массив съест память. */
	struct FCellEmitter
	{
		explicit FCellEmitter(TArray<FIntVector>& InCells, int64 InMaxCells)
			: Cells(InCells)
			// TArray адресуется int32, так что потолок в любом случае не может
			// быть выше MAX_int32 - сколько бы ни выставили в настройках.
			, MaxCells(FMath::Min(InMaxCells, static_cast<int64>(MAX_int32)))
		{
		}

		FORCEINLINE bool Emit(const FIntVector& Cell)
		{
			if (static_cast<int64>(Cells.Num()) >= MaxCells)
			{
				bOverflow = true;
				return false;
			}

			Cells.Add(Cell);
			return true;
		}

		FORCEINLINE bool Emit(int32 X, int32 Y, int32 Z)
		{
			return Emit(FIntVector(X, Y, Z));
		}

		TArray<FIntVector>& Cells;
		int64 MaxCells;
		bool bOverflow = false;
	};

	/** Убирает повторы на месте. Нужен там, где один и тот же узел строится
	 *  дважды по построению: пересечения плит и балок разных осей, клетки на
	 *  плоскостях симметрии. Сортировкой, а не через TSet: на миллионах клеток
	 *  TSet стоил бы сотни мегабайт сверх самого массива. */
	void SortAndDedupe(TArray<FIntVector>& Cells)
	{
		Algo::Sort(Cells, [](const FIntVector& A, const FIntVector& B)
		{
			if (A.Z != B.Z) { return A.Z < B.Z; }
			if (A.Y != B.Y) { return A.Y < B.Y; }
			return A.X < B.X;
		});

		Cells.SetNum(static_cast<int32>(Algo::Unique(Cells)), EAllowShrinking::No);
	}

	// --- Решётка / кристалл -------------------------------------------------

	void GenerateLatticeBlocks(const FStateGeneratorParams& Params, FCellEmitter& Emitter)
	{
		const FIntVector& E = Params.Extent;
		const FIntVector& P = Params.Period;
		const int32 Block = Params.BlockSize;

		// Смещение на полблока одинаково во всех узлах, поэтому решётка
		// остаётся симметричной относительно нуля и при чётном BlockSize.
		const int32 Shift = Block / 2;

		const int32 NodesX = NodeHalfCount(E.X, P.X);
		const int32 NodesY = NodeHalfCount(E.Y, P.Y);
		const int32 NodesZ = NodeHalfCount(E.Z, P.Z);

		for (int32 nz = -NodesZ; nz <= NodesZ; ++nz)
		{
			for (int32 ny = -NodesY; ny <= NodesY; ++ny)
			{
				for (int32 nx = -NodesX; nx <= NodesX; ++nx)
				{
					const FIntVector Base(nx * P.X - Shift, ny * P.Y - Shift, nz * P.Z - Shift);

					for (int32 dz = 0; dz < Block; ++dz)
					{
						for (int32 dy = 0; dy < Block; ++dy)
						{
							for (int32 dx = 0; dx < Block; ++dx)
							{
								const FIntVector Cell(Base.X + dx, Base.Y + dy, Base.Z + dz);
								if (FMath::Abs(Cell.X) > E.X || FMath::Abs(Cell.Y) > E.Y || FMath::Abs(Cell.Z) > E.Z)
								{
									continue;
								}

								if (!Emitter.Emit(Cell))
								{
									return;
								}
							}
						}
					}
				}
			}
		}
	}

	void GenerateLatticeCheckerboard(const FStateGeneratorParams& Params, FCellEmitter& Emitter)
	{
		const FIntVector& E = Params.Extent;
		const int32 S = FMath::Max(Params.BlockSize, 1);

		for (int32 z = -E.Z; z <= E.Z; ++z)
		{
			for (int32 y = -E.Y; y <= E.Y; ++y)
			{
				for (int32 x = -E.X; x <= E.X; ++x)
				{
					const int32 Parity = FloorDivInt(x, S) + FloorDivInt(y, S) + FloorDivInt(z, S);
					if ((WrapMod(Parity, 2)) != 0)
					{
						continue;
					}

					if (!Emitter.Emit(x, y, z))
					{
						return;
					}
				}
			}
		}
	}

	/** Плиты толщиной Thickness с шагом Period, перпендикулярные каждой из
	 *  включённых осей. Строится по узлам, а не сканом объёма: при большом
	 *  шаге скан перебирал бы миллиарды клеток ради считанных плит. */
	void GenerateLatticePlanes(const FStateGeneratorParams& Params, FCellEmitter& Emitter)
	{
		const FIntVector& E = Params.Extent;
		const FIntVector& P = Params.Period;
		const int32 T = Params.Thickness;
		const int32 Shift = T / 2;

		const bool bAxes[3] = { Params.bAxisX, Params.bAxisY, Params.bAxisZ };
		const int32 ExtentAxis[3] = { E.X, E.Y, E.Z };
		const int32 PeriodAxis[3] = { P.X, P.Y, P.Z };

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!bAxes[Axis])
			{
				continue;
			}

			// Две другие оси - те, вдоль которых плита бесконечна.
			const int32 AxisB = (Axis + 1) % 3;
			const int32 AxisC = (Axis + 2) % 3;

			const int32 Nodes = NodeHalfCount(ExtentAxis[Axis], PeriodAxis[Axis]);

			for (int32 Node = -Nodes; Node <= Nodes; ++Node)
			{
				for (int32 t = 0; t < T; ++t)
				{
					const int32 Slab = Node * PeriodAxis[Axis] + t - Shift;
					if (FMath::Abs(Slab) > ExtentAxis[Axis])
					{
						continue;
					}

					for (int32 c = -ExtentAxis[AxisC]; c <= ExtentAxis[AxisC]; ++c)
					{
						for (int32 b = -ExtentAxis[AxisB]; b <= ExtentAxis[AxisB]; ++b)
						{
							FIntVector Cell(0, 0, 0);
							Cell[Axis] = Slab;
							Cell[AxisB] = b;
							Cell[AxisC] = c;

							if (!Emitter.Emit(Cell))
							{
								return;
							}
						}
					}
				}
			}
		}
	}

	/** Каркас: балка вдоль оси лежит на пересечении линий решётки по двум
	 *  другим осям. Включённая ось означает "балки вдоль этой оси". */
	void GenerateLatticeFrame(const FStateGeneratorParams& Params, FCellEmitter& Emitter)
	{
		const FIntVector& E = Params.Extent;
		const FIntVector& P = Params.Period;
		const int32 T = Params.Thickness;
		const int32 Shift = T / 2;

		const bool bAxes[3] = { Params.bAxisX, Params.bAxisY, Params.bAxisZ };
		const int32 ExtentAxis[3] = { E.X, E.Y, E.Z };
		const int32 PeriodAxis[3] = { P.X, P.Y, P.Z };

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!bAxes[Axis])
			{
				continue;
			}

			const int32 AxisB = (Axis + 1) % 3;
			const int32 AxisC = (Axis + 2) % 3;

			const int32 NodesB = NodeHalfCount(ExtentAxis[AxisB], PeriodAxis[AxisB]);
			const int32 NodesC = NodeHalfCount(ExtentAxis[AxisC], PeriodAxis[AxisC]);

			for (int32 NodeC = -NodesC; NodeC <= NodesC; ++NodeC)
			{
				for (int32 tc = 0; tc < T; ++tc)
				{
					const int32 CoordC = NodeC * PeriodAxis[AxisC] + tc - Shift;
					if (FMath::Abs(CoordC) > ExtentAxis[AxisC])
					{
						continue;
					}

					for (int32 NodeB = -NodesB; NodeB <= NodesB; ++NodeB)
					{
						for (int32 tb = 0; tb < T; ++tb)
						{
							const int32 CoordB = NodeB * PeriodAxis[AxisB] + tb - Shift;
							if (FMath::Abs(CoordB) > ExtentAxis[AxisB])
							{
								continue;
							}

							for (int32 a = -ExtentAxis[Axis]; a <= ExtentAxis[Axis]; ++a)
							{
								FIntVector Cell(0, 0, 0);
								Cell[Axis] = a;
								Cell[AxisB] = CoordB;
								Cell[AxisC] = CoordC;

								if (!Emitter.Emit(Cell))
								{
									return;
								}
							}
						}
					}
				}
			}
		}
	}

	// --- Заполненные тела ---------------------------------------------------

	void GenerateSolidBox(const FStateGeneratorParams& Params, FCellEmitter& Emitter)
	{
		const FIntVector& E = Params.Extent;

		for (int32 z = -E.Z; z <= E.Z; ++z)
		{
			for (int32 y = -E.Y; y <= E.Y; ++y)
			{
				for (int32 x = -E.X; x <= E.X; ++x)
				{
					if (!Emitter.Emit(x, y, z))
					{
						return;
					}
				}
			}
		}
	}

	void GenerateBoxShell(const FStateGeneratorParams& Params, FCellEmitter& Emitter)
	{
		const FIntVector& E = Params.Extent;
		const int32 T = Params.Thickness;

		// Внутренняя полость - тот же ящик, ужатый на толщину стенки.
		const FIntVector Inner(E.X - T, E.Y - T, E.Z - T);

		for (int32 z = -E.Z; z <= E.Z; ++z)
		{
			for (int32 y = -E.Y; y <= E.Y; ++y)
			{
				for (int32 x = -E.X; x <= E.X; ++x)
				{
					const bool bInsideCavity = FMath::Abs(x) <= Inner.X && FMath::Abs(y) <= Inner.Y && FMath::Abs(z) <= Inner.Z;
					if (bInsideCavity)
					{
						continue;
					}

					if (!Emitter.Emit(x, y, z))
					{
						return;
					}
				}
			}
		}
	}

	void GenerateSphere(const FStateGeneratorParams& Params, bool bShell, FCellEmitter& Emitter)
	{
		const int32 R = Params.Radius;
		const int32 Inner = bShell ? FMath::Max(R - Params.Thickness, 0) : 0;

		// Целочисленно, без sqrt: сравниваем квадраты.
		const int64 OuterSq = static_cast<int64>(R) * R;
		const int64 InnerSq = static_cast<int64>(Inner) * Inner;

		for (int32 z = -R; z <= R; ++z)
		{
			for (int32 y = -R; y <= R; ++y)
			{
				for (int32 x = -R; x <= R; ++x)
				{
					const int64 DistSq = static_cast<int64>(x) * x + static_cast<int64>(y) * y + static_cast<int64>(z) * z;
					if (DistSq > OuterSq || (bShell && DistSq < InnerSq))
					{
						continue;
					}

					if (!Emitter.Emit(x, y, z))
					{
						return;
					}
				}
			}
		}
	}

	// --- Шум и кластеры -----------------------------------------------------

	void GenerateNoiseUniform(const FStateGeneratorParams& Params, int32 Seed, FCellEmitter& Emitter)
	{
		const FIntVector& E = Params.Extent;
		FRandomStream Stream(Seed);

		// Порядок обхода Z -> Y -> X - часть контракта детерминизма: он
		// определяет, в каком порядке расходуется поток случайных чисел.
		for (int32 z = -E.Z; z <= E.Z; ++z)
		{
			for (int32 y = -E.Y; y <= E.Y; ++y)
			{
				for (int32 x = -E.X; x <= E.X; ++x)
				{
					if (Stream.FRand() >= Params.Density)
					{
						continue;
					}

					if (!Emitter.Emit(x, y, z))
					{
						return;
					}
				}
			}
		}
	}

	void GenerateNoisePerlin(const FStateGeneratorParams& Params, int32 Seed, FCellEmitter& Emitter)
	{
		const FIntVector& E = Params.Extent;

		// FMath::PerlinNoise3D() сида не принимает, поэтому "разные сиды" - это
		// разные точки одного и того же поля. Смещение обязано иметь дробную
		// часть: в целочисленных точках шум равен ровно нулю.
		FRandomStream Stream(Seed);
		const FVector Offset(
			Stream.FRandRange(-512.0f, 512.0f),
			Stream.FRandRange(-512.0f, 512.0f),
			Stream.FRandRange(-512.0f, 512.0f));

		const float Scale = Params.NoiseScale;

		// Однопоточно: шум чист по координате, так что распараллелить по
		// Z-плоскостям можно без всякого потока случайных чисел, но генерация
		// разовая, а порядок обхода тогда пришлось бы восстанавливать
		// склейкой. Если станет узким местом - параллелить именно так.
		for (int32 z = -E.Z; z <= E.Z; ++z)
		{
			for (int32 y = -E.Y; y <= E.Y; ++y)
			{
				for (int32 x = -E.X; x <= E.X; ++x)
				{
					const FVector SamplePoint = (FVector(x, y, z) + Offset) * Scale;
					if (FMath::PerlinNoise3D(SamplePoint) <= Params.NoiseThreshold)
					{
						continue;
					}

					if (!Emitter.Emit(x, y, z))
					{
						return;
					}
				}
			}
		}
	}

	void GenerateNoiseClusters(const FStateGeneratorParams& Params, int32 Seed, FCellEmitter& Emitter)
	{
		const FIntVector& E = Params.Extent;

		for (int32 Index = 0; Index < Params.ClusterCount; ++Index)
		{
			// Свой подпоток на каждое зерно, а не один сквозной: тогда
			// увеличение ClusterCount с 10 до 11 оставляет первые десять зёрен
			// ровно там же, где они были. При сквозном потоке сдвинулось бы всё.
			FRandomStream Stream(static_cast<int32>(HashCombine(static_cast<uint32>(Seed), static_cast<uint32>(Index))));

			const FIntVector Center(
				Stream.RandRange(-E.X, E.X),
				Stream.RandRange(-E.Y, E.Y),
				Stream.RandRange(-E.Z, E.Z));

			const float Jitter = Stream.FRandRange(-Params.ClusterRadiusJitter, Params.ClusterRadiusJitter);
			const int32 R = FMath::Max(FMath::RoundToInt(Params.ClusterRadius * (1.0f + Jitter)), 1);
			const int64 RadiusSq = static_cast<int64>(R) * R;

			for (int32 dz = -R; dz <= R; ++dz)
			{
				for (int32 dy = -R; dy <= R; ++dy)
				{
					for (int32 dx = -R; dx <= R; ++dx)
					{
						const int64 DistSq = static_cast<int64>(dx) * dx + static_cast<int64>(dy) * dy + static_cast<int64>(dz) * dz;
						if (DistSq > RadiusSq)
						{
							continue;
						}

						if (Stream.FRand() >= Params.Density)
						{
							continue;
						}

						const FIntVector Cell(Center.X + dx, Center.Y + dy, Center.Z + dz);
						if (FMath::Abs(Cell.X) > E.X || FMath::Abs(Cell.Y) > E.Y || FMath::Abs(Cell.Z) > E.Z)
						{
							continue;
						}

						if (!Emitter.Emit(Cell))
						{
							return;
						}
					}
				}
			}
		}
	}

	// --- Симметричные затравки ----------------------------------------------

	/** Сколько образов даёт преобразование - оценка сверху: клетки на
	 *  плоскостях и осях симметрии переходят сами в себя, поэтому после
	 *  дедупликации их всегда меньше. */
	int32 SymmetryImageCount(ESeedSymmetry Symmetry)
	{
		switch (Symmetry)
		{
		case ESeedSymmetry::None:       return 1;
		case ESeedSymmetry::MirrorX:    return 2;
		case ESeedSymmetry::MirrorXY:   return 4;
		case ESeedSymmetry::MirrorXYZ:  return 8;
		case ESeedSymmetry::RotateZ4:   return 4;
		case ESeedSymmetry::FullCubic:  return 48;
		default:                        return 1;
		}
	}

	void AppendSymmetryImages(const FIntVector& Cell, ESeedSymmetry Symmetry, TArray<FIntVector>& Out)
	{
		switch (Symmetry)
		{
		case ESeedSymmetry::None:
			Out.Add(Cell);
			break;

		case ESeedSymmetry::MirrorX:
			for (int32 sx = 1; sx >= -1; sx -= 2)
			{
				Out.Add(FIntVector(sx * Cell.X, Cell.Y, Cell.Z));
			}
			break;

		case ESeedSymmetry::MirrorXY:
			for (int32 sy = 1; sy >= -1; sy -= 2)
			{
				for (int32 sx = 1; sx >= -1; sx -= 2)
				{
					Out.Add(FIntVector(sx * Cell.X, sy * Cell.Y, Cell.Z));
				}
			}
			break;

		case ESeedSymmetry::MirrorXYZ:
			for (int32 sz = 1; sz >= -1; sz -= 2)
			{
				for (int32 sy = 1; sy >= -1; sy -= 2)
				{
					for (int32 sx = 1; sx >= -1; sx -= 2)
					{
						Out.Add(FIntVector(sx * Cell.X, sy * Cell.Y, sz * Cell.Z));
					}
				}
			}
			break;

		case ESeedSymmetry::RotateZ4:
		{
			// (x, y) -> (-y, x), четырежды; высота не меняется.
			int32 x = Cell.X;
			int32 y = Cell.Y;
			for (int32 Turn = 0; Turn < 4; ++Turn)
			{
				Out.Add(FIntVector(x, y, Cell.Z));
				const int32 NewX = -y;
				y = x;
				x = NewX;
			}
			break;
		}

		case ESeedSymmetry::FullCubic:
		{
			// Полная группа симметрий куба: 6 перестановок осей на 8
			// комбинаций знаков.
			static const int32 Permutations[6][3] = {
				{ 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 },
				{ 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 }
			};

			const int32 Source[3] = { Cell.X, Cell.Y, Cell.Z };

			for (int32 Perm = 0; Perm < 6; ++Perm)
			{
				for (int32 Signs = 0; Signs < 8; ++Signs)
				{
					FIntVector Image(0, 0, 0);
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						const int32 Sign = (Signs & (1 << Axis)) ? -1 : 1;
						Image[Axis] = Sign * Source[Permutations[Perm][Axis]];
					}
					Out.Add(Image);
				}
			}
			break;
		}

		default:
			Out.Add(Cell);
			break;
		}
	}

	void GenerateSymmetricSeed(const FStateGeneratorParams& Params, int32 Seed, FCellEmitter& Emitter)
	{
		const FIntVector& Core = Params.CoreExtent;
		FRandomStream Stream(Seed);

		TArray<FIntVector> Images;
		Images.Reserve(SymmetryImageCount(Params.Symmetry));

		// Ядро строится в положительном октанте, ВКЛЮЧАЯ нулевые плоскости -
		// именно клетки на них и дают самопересечения при размножении, ради
		// которых результат обязан дедуплицироваться.
		for (int32 z = 0; z <= Core.Z; ++z)
		{
			for (int32 y = 0; y <= Core.Y; ++y)
			{
				for (int32 x = 0; x <= Core.X; ++x)
				{
					if (Stream.FRand() >= Params.Density)
					{
						continue;
					}

					Images.Reset();
					AppendSymmetryImages(FIntVector(x, y, z), Params.Symmetry, Images);

					for (const FIntVector& Image : Images)
					{
						if (!Emitter.Emit(Image))
						{
							return;
						}
					}
				}
			}
		}
	}

	/** Порождает ли генератор повторы по построению. */
	bool NeedsDedupe(const FStateGeneratorParams& Params)
	{
		switch (Params.Type)
		{
		case EStateGeneratorType::SymmetricSeed:
			return Params.Symmetry != ESeedSymmetry::None;

		case EStateGeneratorType::LatticePlanes:
		case EStateGeneratorType::LatticeFrame:
		{
			// Пересечения возникают только между семействами РАЗНЫХ осей.
			const int32 AxisCount = (Params.bAxisX ? 1 : 0) + (Params.bAxisY ? 1 : 0) + (Params.bAxisZ ? 1 : 0);
			return AxisCount > 1;
		}

		case EStateGeneratorType::NoiseClusters:
			// Зёрна перекрываются.
			return true;

		default:
			return false;
		}
	}
}

namespace StateGenerators
{
	bool Generate(const FStateGeneratorParams& Params, int32 Seed, int64 MaxCells,
				  TArray<FIntVector>& OutCells, FGenerateStats& OutStats, FString& OutError)
	{
		const double StartSeconds = FPlatformTime::Seconds();

		// Собираем во внутренний массив и отдаём наружу только при успехе -
		// неудачная генерация не должна оставлять вызывающего с
		// полу-построенным набором.
		TArray<FIntVector> Cells;

		const int64 Estimate = EstimateCellCount(Params);
		if (Estimate > 0 && Estimate <= MaxCells)
		{
			Cells.Reserve(static_cast<int32>(FMath::Min(Estimate, static_cast<int64>(MAX_int32))));
		}

		FCellEmitter Emitter(Cells, MaxCells);

		switch (Params.Type)
		{
		case EStateGeneratorType::RandomBall:
		{
			// Дословно тот же цикл, что был в GenerateRandom() до появления
			// генераторов: тот же порядок обращений к потоку, тот же
			// reject-sampling, то же округление. Один и тот же Seed обязан
			// давать ту же картинку, что и раньше, - это проверяет
			// автотест Generation.RandomBallParity.
			FRandomStream RandomStream(Seed);
			const float RadiusInCells = static_cast<float>(Params.Radius);

			for (int32 i = 0; i < Params.Amount; ++i)
			{
				FVector SamplePoint;
				do
				{
					SamplePoint = FVector(
						RandomStream.FRandRange(-RadiusInCells, RadiusInCells),
						RandomStream.FRandRange(-RadiusInCells, RadiusInCells),
						RandomStream.FRandRange(-RadiusInCells, RadiusInCells));
				}
				while (SamplePoint.SizeSquared() > FMath::Square(RadiusInCells));

				const FIntVector GridCell(
					FMath::RoundToInt(SamplePoint.X),
					FMath::RoundToInt(SamplePoint.Y),
					FMath::RoundToInt(SamplePoint.Z));

				if (!Emitter.Emit(GridCell))
				{
					break;
				}
			}
			break;
		}

		case EStateGeneratorType::LatticeBlocks:
			GenerateLatticeBlocks(Params, Emitter);
			break;

		case EStateGeneratorType::LatticeCheckerboard:
			GenerateLatticeCheckerboard(Params, Emitter);
			break;

		case EStateGeneratorType::LatticeFrame:
			GenerateLatticeFrame(Params, Emitter);
			break;

		case EStateGeneratorType::LatticePlanes:
			GenerateLatticePlanes(Params, Emitter);
			break;

		case EStateGeneratorType::SolidBox:
			GenerateSolidBox(Params, Emitter);
			break;

		case EStateGeneratorType::SolidSphere:
			GenerateSphere(Params, /*bShell=*/false, Emitter);
			break;

		case EStateGeneratorType::SphereShell:
			GenerateSphere(Params, /*bShell=*/true, Emitter);
			break;

		case EStateGeneratorType::BoxShell:
			GenerateBoxShell(Params, Emitter);
			break;

		case EStateGeneratorType::NoiseUniform:
			GenerateNoiseUniform(Params, Seed, Emitter);
			break;

		case EStateGeneratorType::NoisePerlin:
			GenerateNoisePerlin(Params, Seed, Emitter);
			break;

		case EStateGeneratorType::NoiseClusters:
			GenerateNoiseClusters(Params, Seed, Emitter);
			break;

		case EStateGeneratorType::SymmetricSeed:
			GenerateSymmetricSeed(Params, Seed, Emitter);
			break;

		default:
			OutError = FString::Printf(TEXT("неизвестный тип генератора (%d)"), static_cast<int32>(Params.Type));
			return false;
		}

		if (Emitter.bOverflow)
		{
			OutError = FString::Printf(
				TEXT("генератор '%s' перевалил за предел в %lld клеток - уменьшите область или поднимите MaxGeneratedCells"),
				*GetDisplayName(Params.Type), MaxCells);
			return false;
		}

		if (NeedsDedupe(Params))
		{
			SortAndDedupe(Cells);
		}

		if (Cells.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("генератор '%s' не дал ни одной клетки - проверьте параметры"),
				*GetDisplayName(Params.Type));
			return false;
		}

		OutStats.EmittedCells = Cells.Num();
		OutStats.ScannedCells = EstimateScannedCells(Params);
		OutStats.Seconds = FPlatformTime::Seconds() - StartSeconds;

		OutCells = MoveTemp(Cells);
		return true;
	}

	int64 EstimateCellCount(const FStateGeneratorParams& Params)
	{
		const FIntVector& E = Params.Extent;
		const FIntVector& P = Params.Period;

		switch (Params.Type)
		{
		case EStateGeneratorType::RandomBall:
			// Бросков ровно Amount, но часть попадёт в одну клетку - это
			// оценка сверху.
			return Params.Amount;

		case EStateGeneratorType::LatticeBlocks:
		{
			const int64 Nodes = (2LL * NodeHalfCount(E.X, P.X) + 1)
							  * (2LL * NodeHalfCount(E.Y, P.Y) + 1)
							  * (2LL * NodeHalfCount(E.Z, P.Z) + 1);
			const int64 Block = static_cast<int64>(Params.BlockSize) * Params.BlockSize * Params.BlockSize;
			return Nodes * Block;
		}

		case EStateGeneratorType::LatticeCheckerboard:
			return BoxVolume(E) / 2 + 1;

		case EStateGeneratorType::LatticePlanes:
		{
			const bool bAxes[3] = { Params.bAxisX, Params.bAxisY, Params.bAxisZ };
			const int32 ExtentAxis[3] = { E.X, E.Y, E.Z };
			const int32 PeriodAxis[3] = { P.X, P.Y, P.Z };

			int64 Total = 0;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (!bAxes[Axis])
				{
					continue;
				}

				const int64 Slabs = (2LL * NodeHalfCount(ExtentAxis[Axis], PeriodAxis[Axis]) + 1) * Params.Thickness;
				Total += Slabs * AxisDim(ExtentAxis[(Axis + 1) % 3]) * AxisDim(ExtentAxis[(Axis + 2) % 3]);
			}
			return Total;
		}

		case EStateGeneratorType::LatticeFrame:
		{
			const bool bAxes[3] = { Params.bAxisX, Params.bAxisY, Params.bAxisZ };
			const int32 ExtentAxis[3] = { E.X, E.Y, E.Z };
			const int32 PeriodAxis[3] = { P.X, P.Y, P.Z };

			int64 Total = 0;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (!bAxes[Axis])
				{
					continue;
				}

				const int32 AxisB = (Axis + 1) % 3;
				const int32 AxisC = (Axis + 2) % 3;

				const int64 LinesB = (2LL * NodeHalfCount(ExtentAxis[AxisB], PeriodAxis[AxisB]) + 1) * Params.Thickness;
				const int64 LinesC = (2LL * NodeHalfCount(ExtentAxis[AxisC], PeriodAxis[AxisC]) + 1) * Params.Thickness;
				Total += LinesB * LinesC * AxisDim(ExtentAxis[Axis]);
			}
			return Total;
		}

		case EStateGeneratorType::SolidBox:
			return BoxVolume(E);

		case EStateGeneratorType::BoxShell:
		{
			const FIntVector Inner(E.X - Params.Thickness, E.Y - Params.Thickness, E.Z - Params.Thickness);
			const int64 Cavity = (Inner.X < 0 || Inner.Y < 0 || Inner.Z < 0) ? 0 : BoxVolume(Inner);
			return BoxVolume(E) - Cavity;
		}

		case EStateGeneratorType::SolidSphere:
			// Точно, а не по формуле объёма - см. CountBallLatticePoints().
			return CountBallLatticePoints(Params.Radius);

		case EStateGeneratorType::SphereShell:
		{
			// Оболочка = шар минус полость, и вычитается ровно то, что
			// отбрасывает генератор: клетки со СТРОГО меньшим квадратом
			// расстояния, то есть попадающие в InnerSq - 1.
			const int32 Inner = FMath::Max(Params.Radius - Params.Thickness, 0);
			const int64 Cavity = (Inner > 0) ? CountLatticePointsWithinSq(static_cast<int64>(Inner) * Inner - 1) : 0;
			return CountBallLatticePoints(Params.Radius) - Cavity;
		}

		case EStateGeneratorType::NoiseUniform:
			return static_cast<int64>(BoxVolume(E) * Params.Density);

		case EStateGeneratorType::NoisePerlin:
			// Доля заполнения зависит от порога нелинейно - берём половину
			// объёма как грубую верхнюю оценку; фактический предел всё равно
			// проверяется в ходе работы.
			return BoxVolume(E) / 2;

		case EStateGeneratorType::NoiseClusters:
		{
			// Ожидаемая, не верхняя: заполнение зерна вероятностное, а
			// перекрытия зёрен не вычтены. Фактический предел всё равно
			// проверяется по ходу работы.
			const int32 MaxRadius = FMath::RoundToInt(Params.ClusterRadius * (1.0f + Params.ClusterRadiusJitter));
			return static_cast<int64>(Params.ClusterCount * CountBallLatticePoints(MaxRadius) * Params.Density);
		}

		case EStateGeneratorType::SymmetricSeed:
		{
			const int64 CoreVolume = static_cast<int64>(Params.CoreExtent.X + 1)
								   * (Params.CoreExtent.Y + 1)
								   * (Params.CoreExtent.Z + 1);
			return static_cast<int64>(CoreVolume * Params.Density) * SymmetryImageCount(Params.Symmetry);
		}

		default:
			return 0;
		}
	}

	int64 EstimateScannedCells(const FStateGeneratorParams& Params)
	{
		switch (Params.Type)
		{
		case EStateGeneratorType::LatticeCheckerboard:
		case EStateGeneratorType::SolidBox:
		case EStateGeneratorType::BoxShell:
		case EStateGeneratorType::NoiseUniform:
		case EStateGeneratorType::NoisePerlin:
			return BoxVolume(Params.Extent);

		case EStateGeneratorType::SolidSphere:
		case EStateGeneratorType::SphereShell:
		{
			const int64 Side = 2LL * Params.Radius + 1;
			return Side * Side * Side;
		}

		case EStateGeneratorType::NoiseClusters:
		{
			const double MaxRadius = Params.ClusterRadius * (1.0 + Params.ClusterRadiusJitter);
			const int64 Side = 2LL * FMath::RoundToInt(MaxRadius) + 1;
			return Params.ClusterCount * Side * Side * Side;
		}

		default:
			// Конструктивные генераторы перебирают ровно то, что порождают.
			return EstimateCellCount(Params);
		}
	}

	FString GetDisplayName(EStateGeneratorType Type)
	{
		switch (Type)
		{
		case EStateGeneratorType::RandomBall:          return TEXT("случайный шар");
		case EStateGeneratorType::LatticeBlocks:       return TEXT("решётка: блоки");
		case EStateGeneratorType::LatticeCheckerboard: return TEXT("решётка: шахматная");
		case EStateGeneratorType::LatticeFrame:        return TEXT("решётка: каркас");
		case EStateGeneratorType::LatticePlanes:       return TEXT("решётка: плоскости");
		case EStateGeneratorType::SolidBox:            return TEXT("тело: куб");
		case EStateGeneratorType::SolidSphere:         return TEXT("тело: шар");
		case EStateGeneratorType::SphereShell:         return TEXT("тело: полый шар");
		case EStateGeneratorType::BoxShell:            return TEXT("тело: полый куб");
		case EStateGeneratorType::NoiseUniform:        return TEXT("шум: равномерный");
		case EStateGeneratorType::NoisePerlin:         return TEXT("шум: Perlin");
		case EStateGeneratorType::NoiseClusters:       return TEXT("шум: кластеры");
		case EStateGeneratorType::SymmetricSeed:       return TEXT("симметричная затравка");
		default:                                       return TEXT("неизвестный");
		}
	}

	void AnalyzeNeighborCounts(const TArray<FIntVector>& Cells, ENeighborhood Neighborhood,
							   int32 MaxSampleExtent, FNeighborHistogram& OutHistogram)
	{
		// Смещения соседей берутся у самого правила: это чистая геометрия
		// (никакие BirthCounts/SurvivalCounts здесь не читаются), и раньше
		// тут лежал её дубликат - с обратным порядком циклов и без всякого
		// понятия о радиусе. Считать гистограмму по одной таблице, а
		// симуляцию по другой - ровно тот способ разъехаться, ради которого
		// эта функция и существует.
		const TArray<FIntVector> Offsets = FCellularAutomatonRule::BuildNeighborOffsets(Neighborhood);

		// Колонка на каждое возможное число соседей плюс колонка нуля - для
		// привычного Moore-26 это прежние 27.
		OutHistogram.AliveByCount.Init(0, Offsets.Num() + 1);
		OutHistogram.EmptyByCount.Init(0, Offsets.Num() + 1);
		OutHistogram.SampledAlive = 0;
		OutHistogram.SampledEmpty = 0;

		if (Cells.Num() == 0)
		{
			return;
		}

		// Весь набор в TSet на миллионах клеток стоил бы сотни мегабайт, а
		// структуры периодичны - центрального подкуба достаточно. Соседи
		// считаются по ПОЛНОМУ набору, а выборкой ограничены только те клетки,
		// для которых считается ответ, иначе на границе выборки счётчики
		// оказались бы занижены.
		TSet<FIntVector> Occupied;
		Occupied.Reserve(Cells.Num());
		for (const FIntVector& Cell : Cells)
		{
			Occupied.Add(Cell);
		}

		const int32 Limit = FMath::Max(MaxSampleExtent, 1);

		TSet<FIntVector> CountedEmpty;

		for (const FIntVector& Cell : Cells)
		{
			if (FMath::Abs(Cell.X) > Limit || FMath::Abs(Cell.Y) > Limit || FMath::Abs(Cell.Z) > Limit)
			{
				continue;
			}

			int32 LiveNeighbors = 0;
			for (const FIntVector& Offset : Offsets)
			{
				if (Occupied.Contains(Cell + Offset))
				{
					++LiveNeighbors;
				}
			}

			++OutHistogram.AliveByCount[LiveNeighbors];
			++OutHistogram.SampledAlive;

			// Примыкающие пустые: каждая считается один раз, сколько бы живых
			// соседей её ни касалось.
			for (const FIntVector& Offset : Offsets)
			{
				const FIntVector Empty = Cell + Offset;
				if (Occupied.Contains(Empty) || CountedEmpty.Contains(Empty))
				{
					continue;
				}
				CountedEmpty.Add(Empty);

				int32 EmptyLiveNeighbors = 0;
				for (const FIntVector& Inner : Offsets)
				{
					if (Occupied.Contains(Empty + Inner))
					{
						++EmptyLiveNeighbors;
					}
				}

				++OutHistogram.EmptyByCount[EmptyLiveNeighbors];
				++OutHistogram.SampledEmpty;
			}
		}
	}

	FString DescribeHistogram(const FNeighborHistogram& Histogram)
	{
		auto FormatRow = [](const TArray<int64>& Counts) -> FString
		{
			FString Row;
			for (int32 Index = 0; Index < Counts.Num(); ++Index)
			{
				if (Counts[Index] == 0)
				{
					continue;
				}

				if (!Row.IsEmpty())
				{
					Row += TEXT(", ");
				}
				Row += FString::Printf(TEXT("%d:%lld"), Index, Counts[Index]);
			}
			return Row.IsEmpty() ? TEXT("-") : Row;
		};

		return FString::Printf(
			TEXT("живые (%lld) [соседей:клеток] %s | примыкающие пустые (%lld) [соседей:клеток] %s"),
			Histogram.SampledAlive, *FormatRow(Histogram.AliveByCount),
			Histogram.SampledEmpty, *FormatRow(Histogram.EmptyByCount));
	}
}
