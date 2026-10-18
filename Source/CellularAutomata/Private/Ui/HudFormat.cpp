#include "Ui/HudFormat.h"

namespace
{
	FText Invariant(const FString& String)
	{
		return FText::AsCultureInvariant(String);
	}

	/** "3.2 / 8.0 GB" из мегабайт; пустая сводка (Total == 0, драйвер не
	 *  ответил) - прочерк, а не "0.0 / 0.0". */
	FString MemoryPair(double UsedMB, double TotalMB)
	{
		if (TotalMB <= 0.0)
		{
			return TEXT("—");
		}
		return FString::Printf(TEXT("%.1f / %.1f GB"), UsedMB / 1024.0, TotalMB / 1024.0);
	}

	bool Fraction(double Part, double Whole, float& Out)
	{
		if (Whole <= 0.0)
		{
			return false;
		}
		Out = static_cast<float>(FMath::Clamp(Part / Whole, 0.0, 1.0));
		return true;
	}

	EHudChipState ArmedOrActive(bool bEnabled, bool bActive)
	{
		if (bActive)
		{
			return EHudChipState::Active;
		}
		return bEnabled ? EHudChipState::Armed : EHudChipState::Off;
	}

	EHudChipState OnOff(bool bOn)
	{
		return bOn ? EHudChipState::Active : EHudChipState::Off;
	}
}

namespace HudFormat
{
	FString GroupedInteger(int64 Value)
	{
		const bool bNegative = Value < 0;
		// Через uint64, чтобы INT64_MIN не переполнился при смене знака.
		const uint64 Magnitude = bNegative ? static_cast<uint64>(-(Value + 1)) + 1 : static_cast<uint64>(Value);
		const FString Digits = FString::Printf(TEXT("%llu"), Magnitude);

		FString Result;
		const int32 Length = Digits.Len();
		for (int32 Index = 0; Index < Length; ++Index)
		{
			if (Index > 0 && (Length - Index) % 3 == 0)
			{
				Result.AppendChar(TEXT(' '));
			}
			Result.AppendChar(Digits[Index]);
		}
		return bNegative ? TEXT("-") + Result : Result;
	}

	FString CompactCount(int64 Value)
	{
		const double Magnitude = FMath::Abs(static_cast<double>(Value));
		if (Magnitude < 10000.0)
		{
			return GroupedInteger(Value);
		}

		static const TCHAR* Units[] = { TEXT("K"), TEXT("M"), TEXT("B"), TEXT("T") };
		const int32 LastUnit = UE_ARRAY_COUNT(Units) - 1;

		double Scaled = Magnitude / 1000.0;
		int32 Unit = 0;
		for (;;)
		{
			// Три значащие цифры: сколько знаков после точки, зависит от
			// величины мантиссы. Единицу переключаем после округления - иначе
			// 999.95K напечаталось бы как "1000K".
			const double Step = Scaled < 10.0 ? 100.0 : (Scaled < 100.0 ? 10.0 : 1.0);
			const double Rounded = FMath::RoundToDouble(Scaled * Step) / Step;
			if (Rounded >= 1000.0 && Unit < LastUnit)
			{
				Scaled /= 1000.0;
				++Unit;
				continue;
			}

			FString Mantissa;
			if (Rounded < 10.0)
			{
				Mantissa = FString::Printf(TEXT("%.2f"), Rounded);
			}
			else if (Rounded < 100.0)
			{
				Mantissa = FString::Printf(TEXT("%.1f"), Rounded);
			}
			else
			{
				Mantissa = FString::Printf(TEXT("%.0f"), Rounded);
			}
			return (Value < 0 ? TEXT("-") : TEXT("")) + Mantissa + Units[Unit];
		}
	}

	FString Rate(double Value)
	{
		if (FMath::Abs(Value) < 100.0)
		{
			return FString::Printf(TEXT("%.1f"), Value);
		}
		return FString::Printf(TEXT("%.0f"), Value);
	}

	FText MetricLabel(EHudMetric Metric)
	{
		switch (Metric)
		{
		case EHudMetric::Generation:      return INVTEXT("GENERATION");
		case EHudMetric::GenerationRate:  return INVTEXT("GEN/S");
		case EHudMetric::TargetSpeed:     return INVTEXT("TARGET");
		case EHudMetric::Alive:           return INVTEXT("ALIVE");
		case EHudMetric::Selected:        return INVTEXT("SELECTED");
		case EHudMetric::Fps:             return INVTEXT("FPS");
		case EHudMetric::Compute:         return INVTEXT("COMPUTE");
		case EHudMetric::StepsPerRender:  return INVTEXT("STEPS/FRAME");
		case EHudMetric::Upload:          return INVTEXT("UPLOAD");
		case EHudMetric::GpuUpload:       return INVTEXT("GPU UPLOAD");
		case EHudMetric::Vram:            return INVTEXT("VRAM");
		case EHudMetric::SystemMemory:    return INVTEXT("RAM");
		case EHudMetric::DrawnCells:      return INVTEXT("DRAWN");
		case EHudMetric::RenderProfile:   return INVTEXT("PROFILE");
		case EHudMetric::Rule:            return INVTEXT("RULE");
		case EHudMetric::RunState:        return INVTEXT("STATE");
		case EHudMetric::Generator:       return INVTEXT("GENERATOR");
		case EHudMetric::GeneratorCells:  return INVTEXT("SEED SIZE");
		case EHudMetric::Sound:           return INVTEXT("SOUND");
		case EHudMetric::CameraSpeed:     return INVTEXT("CAMERA");
		case EHudMetric::AutoReseed:      return INVTEXT("RESEED");
		}
		return FText::GetEmpty();
	}

	FText MetricValue(EHudMetric Metric, const FHudStatsView& Stats)
	{
		const FHudSimulationStats& Sim = Stats.Simulation;
		const FHudPerformanceStats& Perf = Stats.Performance;

		switch (Metric)
		{
		case EHudMetric::Generation:
			return Invariant(GroupedInteger(Sim.GenerationCount));

		case EHudMetric::GenerationRate:
			return Invariant(Rate(Sim.GenerationsPerSecond));

		case EHudMetric::TargetSpeed:
			return Invariant(Rate(Sim.SimulationSpeed));

		case EHudMetric::Alive:
			return Invariant(CompactCount(Sim.AliveCellCount));

		case EHudMetric::Selected:
			return Invariant(Stats.Camera.SelectedCellCount > 0 ? CompactCount(Stats.Camera.SelectedCellCount) : FString(TEXT("—")));

		case EHudMetric::Fps:
			return Invariant(FString::Printf(TEXT("%.0f"), Perf.CurrentFPS));

		case EHudMetric::Compute:
			if (Perf.ComputeMethod == EComputeMethod::Gpu)
			{
				return Perf.bComputeFellBackToCpu ? INVTEXT("CPU (fallback)") : INVTEXT("GPU");
			}
			return INVTEXT("CPU");

		case EHudMetric::StepsPerRender:
			return Invariant(FString::Printf(TEXT("×%d"), Sim.StepsPerRender));

		case EHudMetric::Upload:
			return Invariant(FString::Printf(TEXT("%.1f MB"), Stats.RenderCounts.EstimatedUploadMB));

		case EHudMetric::GpuUpload:
			return Invariant(FString::Printf(TEXT("%.1f MB"), Perf.EstimatedGpuComputeUploadMB));

		case EHudMetric::Vram:
			return Invariant(MemoryPair(Perf.VideoMemoryUsedMB, Perf.VideoMemoryTotalMB));

		case EHudMetric::SystemMemory:
			return Invariant(MemoryPair(Perf.SystemMemoryTotalMB - Perf.SystemMemoryAvailableMB, Perf.SystemMemoryTotalMB));

		case EHudMetric::DrawnCells:
			return Invariant(CompactCount(Stats.RenderCounts.RenderedCellCount) + TEXT(" / ") + CompactCount(Stats.RenderCounts.TotalCellCount));

		case EHudMetric::RenderProfile:
			if (Stats.Render.RenderPresetName.IsEmpty())
			{
				return INVTEXT("—");
			}
			// Та же звёздочка, что "Performance*" в docs/hud.md: профиль выбран,
			// но хоткей уже поменял что-то из того, чем он владеет.
			return Invariant(Stats.Render.RenderPresetName + (Stats.Render.bRenderPresetModified ? TEXT("*") : TEXT("")));

		case EHudMetric::Rule:
			return Invariant(Sim.RuleString);

		case EHudMetric::RunState:
			if (Sim.bSimulationRunning)
			{
				return INVTEXT("RUNNING");
			}
			return Sim.bFastStepActive ? INVTEXT("FAST STEP") : INVTEXT("PAUSED");

		case EHudMetric::Generator:
			return Invariant(Stats.Generator.StateGeneratorName);

		case EHudMetric::GeneratorCells:
			// "~", а не "≈": шрифт HUD (Exo 2) не обязан иметь математические знаки.
			return Invariant(TEXT("~") + CompactCount(Stats.Generator.EstimatedGeneratorCells));

		case EHudMetric::Sound:
			if (!Stats.Sonification.bSonificationEnabled)
			{
				return INVTEXT("OFF");
			}
			// Звук включён, но пресет не выбран (настроен вручную) - пустая
			// строка читалась бы как "ничего", а это как раз "включено".
			if (Stats.Sonification.SonificationPresetName.IsEmpty())
			{
				return INVTEXT("ON");
			}
			return Invariant(Stats.Sonification.SonificationPresetName);

		case EHudMetric::CameraSpeed:
			return Invariant(FString::Printf(TEXT("%.0f"), Stats.Camera.CameraSpeed));

		case EHudMetric::AutoReseed:
			if (!Sim.bAutoReseedOnExtinction)
			{
				return INVTEXT("OFF");
			}
			return Invariant(FString::Printf(TEXT("#%d"), Sim.AutoReseedCount));
		}
		return FText::GetEmpty();
	}

	bool MetricFraction(EHudMetric Metric, const FHudStatsView& Stats, float& OutFraction)
	{
		const FHudPerformanceStats& Perf = Stats.Performance;
		switch (Metric)
		{
		case EHudMetric::GenerationRate:
			// Факт против цели: недобор - читаемый признак того, что упёрлись
			// в счёт, а не в настройку скорости (см. docs/hud.md).
			return Fraction(Stats.Simulation.GenerationsPerSecond, Stats.Simulation.SimulationSpeed, OutFraction);
		case EHudMetric::Vram:
			return Fraction(Perf.VideoMemoryUsedMB, Perf.VideoMemoryTotalMB, OutFraction);
		case EHudMetric::SystemMemory:
			return Fraction(Perf.SystemMemoryTotalMB - Perf.SystemMemoryAvailableMB, Perf.SystemMemoryTotalMB, OutFraction);
		case EHudMetric::DrawnCells:
			return Fraction(Stats.RenderCounts.RenderedCellCount, Stats.RenderCounts.TotalCellCount, OutFraction);
		default:
			return false;
		}
	}

	FText ChipLabel(EHudChip Chip)
	{
		switch (Chip)
		{
		case EHudChip::Culling:         return INVTEXT("CULL");
		case EHudChip::Volume:          return INVTEXT("VOLUME");
		case EHudChip::Slab:            return INVTEXT("SLAB");
		case EHudChip::AgeFilter:       return INVTEXT("AGE");
		case EHudChip::Ghost:           return INVTEXT("GHOST");
		case EHudChip::Shadows:         return INVTEXT("SHADOWS");
		case EHudChip::Background:      return INVTEXT("BACKDROP");
		case EHudChip::Sound:           return INVTEXT("SOUND");
		case EHudChip::Selection:       return INVTEXT("SELECT");
		case EHudChip::AutoReseed:      return INVTEXT("RESEED");
		case EHudChip::Ortho:           return INVTEXT("ORTHO");
		case EHudChip::Headlight:       return INVTEXT("HEADLIGHT");
		case EHudChip::Chunked:         return INVTEXT("CHUNKED");
		case EHudChip::ComputeFallback: return INVTEXT("CPU FALLBACK");
		}
		return FText::GetEmpty();
	}

	EHudChipState ChipState(EHudChip Chip, const FHudStatsView& Stats)
	{
		const FHudCutStats& Cuts = Stats.Cuts;
		switch (Chip)
		{
		// Три отсечения горят по "режет сейчас", а не по "включено" - см.
		// docs/hud.md: bEnableCellCulling по умолчанию true при нулевых
		// дистанциях, и лампа по выключателю горела бы с первой секунды.
		case EHudChip::Culling:   return ArmedOrActive(Cuts.bCellCullingEnabled, Cuts.bCellCullingActive);
		case EHudChip::Volume:    return ArmedOrActive(Cuts.bRenderCullVolumeEnabled, Cuts.bCullVolumeActive);
		case EHudChip::Slab:      return ArmedOrActive(Cuts.bViewSliceEnabled, Cuts.bViewSliceActive);
		case EHudChip::AgeFilter: return OnOff(Cuts.bAgeFilterActive);
		// Ghost Shape - то же различие: включён, но подробный рендер ещё не
		// заменяет (скорость он покупает только во втором случае).
		case EHudChip::Ghost:      return ArmedOrActive(Stats.Render.bGhostShapeEnabled, Stats.Render.bGhostShapeReplacesDetailedRender);
		case EHudChip::Shadows:    return OnOff(Stats.Render.bCellsCastShadows);
		case EHudChip::Background: return OnOff(Stats.Render.bBackgroundVisible);
		case EHudChip::Sound:      return OnOff(Stats.Sonification.bSonificationEnabled);
		case EHudChip::Selection:  return OnOff(Stats.Camera.bSelectionModeActive);
		case EHudChip::AutoReseed: return OnOff(Stats.Simulation.bAutoReseedOnExtinction);
		case EHudChip::Ortho:      return OnOff(Stats.Camera.bOrthographicCamera);
		case EHudChip::Headlight:  return OnOff(Stats.Camera.bHeadlightEnabled);
		case EHudChip::Chunked:    return OnOff(Stats.Render.bChunkedRenderEnabled);
		case EHudChip::ComputeFallback:
			return Stats.Performance.bComputeFellBackToCpu ? EHudChipState::Warning : EHudChipState::Off;
		}
		return EHudChipState::Off;
	}
}
