#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

class UGenerationGraphWidget;

/** Slate-часть графика поколений: рисует две ломаные по данным, которые
 *  подготавливает владелец (UGenerationGraphWidget).
 *
 *  Лежит в Private, а не в Public: наружу этот класс не нужен никому - в UMG
 *  кладут UWidget-обёртку, а больше на график никто не смотрит.
 *
 *  Почему вообще отдельный Slate-виджет, а не UUserWidget с NativePaint (так
 *  задумывалось сперва): SObjectWidget::OnPaint сначала рисует ВСЕХ детей и
 *  только потом зовёт NativePaint с увеличенным LayerId. Подписи осей,
 *  положенные внутрь такого виджета, оказались бы ПОД графиком, а непрозрачный
 *  фон стёр бы их целиком - пришлось бы рисовать текст из C++ через MakeText,
 *  что убивает всю посылку "вёрстка живёт в UMG". Здесь же это обычный узел
 *  дерева: кладётся в Overlay, поверх ставятся TextBlock'и. */
class SGenerationGraph : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SGenerationGraph) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UGenerationGraphWidget>, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Данные меняются снаружи (оркестратор дописывает замеры), без единой
	 *  Slate-инвалидации - виджет обязан считаться volatile, иначе под
	 *  RetainerBox/InvalidationBox и при Slate.EnableGlobalInvalidation 1
	 *  график замёрзнет на закешированной картинке. Volatile-виджеты из
	 *  кеширования исключаются и перерисовываются каждый кадр. */
	virtual bool ComputeVolatility() const override { return true; }

private:
	/** Обе объявлены обязательно: у SLeafWidget они private и чисто
	 *  виртуальные - без любой из них класс останется абстрактным. */
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

	TWeakObjectPtr<UGenerationGraphWidget> Owner;
};
