#include "Ui/GenerationGraphWidget.h"

#include "Kismet/GameplayStatics.h"
#include "Orchestration/AutomataOrchestrator.h"
#include "Orchestration/GenerationHistory.h"
#include "Ui/SGenerationGraph.h"

#define LOCTEXT_NAMESPACE "GenerationGraphWidget"

UGenerationGraphWidget::UGenerationGraphWidget()
{
	// Иначе виджет не получить переменной в графе WBP - а без этого не вызвать
	// GetWindowYMax()/GetWindowFirstGeneration() для подписей осей.
	bIsVariable = true;
}

TSharedRef<SWidget> UGenerationGraphWidget::RebuildWidget()
{
	MyGraph = SNew(SGenerationGraph).Owner(this);
	return MyGraph.ToSharedRef();
}

void UGenerationGraphWidget::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (MyGraph.IsValid())
	{
		// Правки цветов/толщины в Designer читаются прямо из этого UObject на
		// отрисовке, но сама отрисовка без этого может не случиться.
		MyGraph->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void UGenerationGraphWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MyGraph.Reset();
}

#if WITH_EDITOR
const FText UGenerationGraphWidget::GetPaletteCategory()
{
	return LOCTEXT("PaletteCategory", "Automata");
}
#endif

AAutomataOrchestrator* UGenerationGraphWidget::GetOrchestrator()
{
	if (!IsValid(CachedOrchestrator))
	{
		const UWorld* World = GetWorld();
		CachedOrchestrator = World
			? Cast<AAutomataOrchestrator>(UGameplayStatics::GetActorOfClass(World, AAutomataOrchestrator::StaticClass()))
			: nullptr;
	}

	return CachedOrchestrator;
}

double UGenerationGraphWidget::GetActiveExponent() const
{
	return bNormalizePopulation ? double(NormalizationExponent) : 0.0;
}

void UGenerationGraphWidget::UpdateYRange(double RawMinY, double RawMaxY)
{
	if (!bAutoScaleY)
	{
		// FixedMaxY целочисленный и по смыслу про количество клеток - для
		// нормированного отношения (обычно единицы) он бесполезен, но и не
		// вреден: ручной масштаб на то и ручной.
		CachedYMin = 0.0;
		CachedYMax = FMath::Max(1, FixedMaxY);
		return;
	}

	if (bNormalizePopulation)
	{
		// Отрезок выровнен по "красивому" шагу, и это же работает гистерезисом:
		// он меняется, только когда данные переходят через границу шага.
		GenerationHistory::ComputeNiceRange(RawMinY, RawMaxY, CachedYMin, CachedYMax);
		return;
	}

	CachedYMin = 0.0;

	const double Target = GenerationHistory::NiceCeiling(FMath::Max(RawMaxY, 1.0));

	// Вверх - сразу (иначе пик уехал бы за край кадра), вниз - только когда
	// сырой максимум ушёл ниже половины текущего потолка. Без этого зазора
	// масштаб мигал бы туда-сюда на каждом колебании вокруг границы округления.
	if (Target > CachedYMax || RawMaxY < CachedYMax * 0.5)
	{
		CachedYMax = Target;
	}
}

void UGenerationGraphWidget::BuildDesignTimePoints(const FVector2f& PlotSize, const FVector2f& PlotOrigin,
	TArray<FVector2f>& OutAlivePoints, TArray<FVector2f>& OutRenderedPoints)
{
	constexpr int32 PointCount = 48;

	OutAlivePoints.Reset(PointCount);
	OutRenderedPoints.Reset(PointCount);

	for (int32 Index = 0; Index < PointCount; ++Index)
	{
		const float T = float(Index) / float(PointCount - 1);
		const float X = PlotOrigin.X + T * PlotSize.X;

		// Просто узнаваемая пара кривых: гладкая "всего" и ступенчатая
		// "видимо" - ровно так они и выглядят при StepsPerRender > 1.
		const float Alive = 0.25f + 0.55f * FMath::Sin(T * PI);
		const float Rendered = 0.15f + 0.25f * FMath::FloorToFloat(T * 5.0f) / 5.0f;

		OutAlivePoints.Add(FVector2f(X, PlotOrigin.Y + (1.0f - Alive) * PlotSize.Y));
		OutRenderedPoints.Add(FVector2f(X, PlotOrigin.Y + (1.0f - Rendered) * PlotSize.Y));
	}
}

bool UGenerationGraphWidget::BuildPlotPoints(const FVector2f& PlotSize, const FVector2f& PlotOrigin,
	TArray<FVector2f>& OutAlivePoints, TArray<FVector2f>& OutRenderedPoints)
{
	OutAlivePoints.Reset();
	OutRenderedPoints.Reset();

	if (PlotSize.X <= 0.0f || PlotSize.Y <= 0.0f)
	{
		return false;
	}

	AAutomataOrchestrator* Orchestrator = GetOrchestrator();
	if (!Orchestrator)
	{
		// В Designer оркестратора нет и быть не может - показываем заглушку,
		// иначе виджет пустой прямоугольник и его нечем мерить при вёрстке.
		if (IsDesignTime())
		{
			BuildDesignTimePoints(PlotSize, PlotOrigin, OutAlivePoints, OutRenderedPoints);
			return true;
		}
		return false;
	}

	const TArray<FGenerationSample>& Samples = Orchestrator->GetGenerationSamples();
	const double Exponent = GetActiveExponent();

	// Считается независимо от того, включена ли нормировка: показатель - это
	// самостоятельный результат прогона, который в UMG выводят подписью.
	CachedFittedExponent = GenerationHistory::FitGrowthExponent(Samples);

	double RawMinY = 0.0;
	double RawMaxY = 0.0;
	if (!GenerationHistory::ComputeBounds(Samples, Exponent,
		CachedMinGeneration, CachedMaxGeneration, RawMinY, RawMaxY))
	{
		return false;
	}

	UpdateYRange(RawMinY, RawMaxY);

	GenerationHistory::MapToPoints(Samples, PlotSize, PlotOrigin,
		CachedMinGeneration, CachedMaxGeneration, CachedYMin, CachedYMax,
		bLogScaleY, Exponent, OutAlivePoints, OutRenderedPoints);

	// Одна точка - это не ломаная: MakeLines по ней ничего не нарисует, а
	// звать его дважды впустую незачем.
	return OutAlivePoints.Num() >= 2;
}

#undef LOCTEXT_NAMESPACE
