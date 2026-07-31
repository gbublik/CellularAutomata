#include "Ui/SGenerationGraph.h"

#include "Brushes/SlateColorBrush.h"
#include "Rendering/DrawElements.h"
#include "Ui/GenerationGraphWidget.h"

void SGenerationGraph::Construct(const FArguments& InArgs)
{
	Owner = InArgs._Owner;

	// Тик не нужен: всё, что рисуется, читается прямо в OnPaint из оркестратора.
	// Перерисовку обеспечивает ComputeVolatility(), а не тик.
	SetCanTick(false);
}

FVector2D SGenerationGraph::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	// Просто разумный размер по умолчанию, чтобы виджет было видно сразу после
	// перетаскивания из палитры - реальный задаётся слотом в Designer.
	return FVector2D(256.0, 96.0);
}

int32 SGenerationGraph::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	UGenerationGraphWidget* Widget = Owner.Get();
	if (!Widget)
	{
		return LayerId;
	}

	const ESlateDrawEffect DrawEffects = bParentEnabled
		? ESlateDrawEffect::None
		: ESlateDrawEffect::DisabledEffect;

	// Домножаем на тинт стиля, иначе прозрачность и анимации родительской
	// панели графика не касаются: панель гаснет, а он остаётся непрозрачным.
	const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint();

	// MakeLines/MakeBox принимают точки в ЛОКАЛЬНЫХ координатах виджета -
	// перевод в пространство окна несёт сама PaintGeometry.
	const FPaintGeometry PaintGeometry = AllottedGeometry.ToPaintGeometry();
	const FVector2f LocalSize(AllottedGeometry.GetLocalSize());

	int32 Layer = LayerId;

	if (Widget->BackgroundColor.A > 0.0f)
	{
		static const FSlateColorBrush BackgroundBrush(FLinearColor::White);
		FSlateDrawElement::MakeBox(OutDrawElements, Layer, PaintGeometry, &BackgroundBrush,
			DrawEffects, Widget->BackgroundColor * Tint);
	}
	++Layer;

	// Пол-толщины линии: иначе пик, дошедший до верхней границы, срезается
	// краем виджета ровно наполовину.
	const float Inset = Widget->LineThickness * 0.5f + Widget->Padding;
	const FVector2f PlotOrigin(Inset, Inset);
	const FVector2f PlotSize(
		FMath::Max(0.0f, LocalSize.X - 2.0f * Inset),
		FMath::Max(0.0f, LocalSize.Y - 2.0f * Inset));

	TArray<FVector2f> AlivePoints;
	TArray<FVector2f> RenderedPoints;
	if (Widget->BuildPlotPoints(PlotSize, PlotOrigin, AlivePoints, RenderedPoints))
	{
		// "Видимо" первой, чтобы при совпадении линий (нет отсечения, нет
		// фильтров - обычное начало прогона) сверху осталась "всего": иначе
		// кажется, что зелёная линия пропала.
		FSlateDrawElement::MakeLines(OutDrawElements, Layer, PaintGeometry, MoveTemp(RenderedPoints),
			DrawEffects, Widget->RenderedLineColor * Tint, true, Widget->LineThickness);
		FSlateDrawElement::MakeLines(OutDrawElements, Layer, PaintGeometry, MoveTemp(AlivePoints),
			DrawEffects, Widget->AliveLineColor * Tint, true, Widget->LineThickness);
	}
	++Layer;

	return FMath::Max(LayerId, Layer);
}
