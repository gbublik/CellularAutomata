#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "GenerationGraphWidget.generated.h"

class AAutomataOrchestrator;
class SGenerationGraph;

/**
 * График поколений на HUD: две линии по оси поколений - сколько клеток живо во
 * всей сетке и сколько реально уходит в AddInstances() после куба отсечения,
 * фильтра по возрасту и среза.
 *
 * По одному мгновенному числу (FHudStats::AliveCellCount и
 * FCellRenderStats::RenderedCellCount) не видно ровно того, ради чего прогон и
 * запускается: как правило разгоняет структуру, где выходит на плато, где
 * схлопывается, и насколько куб отсечения реально снимает работу с рендера.
 *
 * Кладётся в WBP_MainHud из палитры (раздел Automata) как обычный виджет -
 * размер, позиция и цвета настраиваются в Designer, как и вся остальная
 * вёрстка HUD. Подписи осей и легенду ставить рядом (например, в Overlay
 * поверх), а не внутрь - у этого виджета детей нет по определению.
 *
 * Данные берёт сам, через ленивый GetActorOfClass() - тот же идиом, что
 * UMainHudWidget::GetOrchestrator() и ARenderCullVolume, и по той же причине:
 * ассет не может держать назначенную в редакторе ссылку на актёр уровня.
 */
UCLASS()
class CELLULARAUTOMATA_API UGenerationGraphWidget : public UWidget
{
	GENERATED_BODY()

public:
	UGenerationGraphWidget();

	/** Линия "всего клеток" - Grid->Num(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph")
	FLinearColor AliveLineColor = FLinearColor(0.25f, 0.85f, 0.40f, 1.0f);

	/** Линия "видимо клеток" - то, что уходит в AddInstances(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph")
	FLinearColor RenderedLineColor = FLinearColor(0.95f, 0.65f, 0.15f, 1.0f);

	/** Подложка. Нулевая альфа - фон не рисуется вовсе. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph")
	FLinearColor BackgroundColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.45f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph", meta = (ClampMin = "0.5", UIMax = "8.0"))
	float LineThickness = 1.5f;

	/** Дополнительный отступ от краёв, поверх пол-толщины линии (её и так
	 *  срезало бы краем виджета на пиках). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph", meta = (ClampMin = "0.0", UIMax = "32.0"))
	float Padding = 2.0f;

	/** Потолок по Y считается по окну (по максимуму ОБОИХ рядов). Иначе
	 *  берётся FixedMaxY. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph")
	bool bAutoScaleY = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph", meta = (ClampMin = "1", EditCondition = "!bAutoScaleY"))
	int32 FixedMaxY = 100000;

	/** Логарифм по Y.
	 *
	 *  Нужен не столько из-за размаха живых клеток (внутри одного окна он
	 *  невелик), сколько из-за КУБА ОТСЕЧЕНИЯ: с ним "видимо" уходит на два-три
	 *  порядка ниже "всего", и в линейном масштабе вторая линия просто ложится
	 *  на ось, показывая ноль вместо содержательной величины. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph")
	bool bLogScaleY = false;

	/** Текущий потолок по Y - для подписи оси в UMG. Уже сглаженный
	 *  ("красивый", с гистерезисом), а не сырой максимум окна: см.
	 *  UpdateYMax(). */
	UFUNCTION(BlueprintPure, Category = "Automata|Graph")
	float GetWindowYMax() const { return float(CachedYMax); }

	UFUNCTION(BlueprintPure, Category = "Automata|Graph")
	int64 GetWindowFirstGeneration() const { return CachedMinGeneration; }

	UFUNCTION(BlueprintPure, Category = "Automata|Graph")
	int64 GetWindowLastGeneration() const { return CachedMaxGeneration; }

	/** Готовит точки обеих ломаных под заданную область. false - рисовать
	 *  нечего (нет оркестратора, пустая история, вырожденная область).
	 *  Зовётся из SGenerationGraph::OnPaint(): OnPaint у Slate-виджета const,
	 *  но UObject за ним - нет, поэтому весь изменяемый кэш (найденный
	 *  оркестратор, сглаженный потолок) живёт здесь и никаких mutable не
	 *  требует. */
	bool BuildPlotPoints(const FVector2f& PlotSize, const FVector2f& PlotOrigin,
		TArray<FVector2f>& OutAlivePoints, TArray<FVector2f>& OutRenderedPoints);

	virtual void SynchronizeProperties() override;

	/** Обязательно: без сброса SWidget переживёт рекомпиляцию WBP и останется
	 *  висеть с указателем на уже мёртвый UObject. */
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

#if WITH_EDITOR
	/** Только под WITH_EDITOR - UWidget::GetPaletteCategory() редакторный, в
	 *  не-редакторной сборке это ошибка линковки. */
	virtual const FText GetPaletteCategory() override;
#endif

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	/** Ленивый резолв с ревалидацией IsValid() на каждый вызов - оркестратора
	 *  может ещё не быть (виджет создаётся раньше) или он мог быть уничтожен. */
	AAutomataOrchestrator* GetOrchestrator();

	/** Сглаживает потолок по Y: сырой максимум, пересчитываемый каждый кадр,
	 *  заставляет всю кривую непрерывно дышать. Округляет до "красивого"
	 *  (1/2/5 * 10^k), растёт немедленно, падает только когда сырой максимум
	 *  ушёл ниже половины текущего - гистерезис, иначе на границе округления
	 *  масштаб мигал бы туда-сюда каждый кадр. */
	void UpdateYMax(int32 RawMaxY);

	/** Синтетическая кривая для Designer: без PIE оркестратора не существует,
	 *  и виджет был бы пустым прямоугольником, который нечем мерить. */
	static void BuildDesignTimePoints(const FVector2f& PlotSize, const FVector2f& PlotOrigin,
		TArray<FVector2f>& OutAlivePoints, TArray<FVector2f>& OutRenderedPoints);

	UPROPERTY(Transient)
	TObjectPtr<AAutomataOrchestrator> CachedOrchestrator = nullptr;

	double CachedYMax = 1.0;
	int64 CachedMinGeneration = 0;
	int64 CachedMaxGeneration = 0;

	TSharedPtr<SGenerationGraph> MyGraph;
};
