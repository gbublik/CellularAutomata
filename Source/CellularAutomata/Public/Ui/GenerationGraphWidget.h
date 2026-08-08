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
 * По одному мгновенному числу (FHudSimulationStats::AliveCellCount и
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

	/** Нормировка: рисовать не популяцию, а популяцию, делённую на
	 *  n^NormalizationExponent (n - номер поколения).
	 *
	 *  Голая популяция растущей фигуры идёт как объём, и её график - парабола,
	 *  одинаковая у любого растущего правила: всё различие спрятано в
	 *  коэффициенте. Деление убирает главный член и показывает то, что он
	 *  прятал, - см. GenerationHistory::NormalizedValue(). Приём взят с графика
	 *  U(n)/n^2 в статье про автомат Улама-Варбертона, где он показывает, что
	 *  отношение не сходится вовсе, а вечно колеблется между 0.9026... и 4/3,
	 *  касаясь потолка ровно при n = 2^k.
	 *
	 *  Выключено по умолчанию: обычный график населения - то, ради чего виджет
	 *  клали на HUD, а нормировка отвечает на отдельный вопрос. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph")
	bool bNormalizePopulation = false;

	/** Показатель нормировки. Тройка по умолчанию - размерность пространства,
	 *  то есть гипотеза "фигура плотная": при ней у сплошного растущего тела
	 *  отношение выходит на константу, а у фрактальной губки уползает в ноль.
	 *  Измеренное значение показывает GetFittedGrowthExponent() - его и имеет
	 *  смысл сюда вписывать, чтобы кривая легла горизонтально и стало видно
	 *  колебания вокруг неё. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automata|Graph",
		meta = (ClampMin = "0.1", UIMax = "4.0", EditCondition = "bNormalizePopulation"))
	float NormalizationExponent = 3.0f;

	/** Текущий потолок по Y - для подписи оси в UMG. Уже сглаженный
	 *  ("красивый", с гистерезисом), а не сырой максимум окна: см.
	 *  UpdateYRange(). */
	UFUNCTION(BlueprintPure, Category = "Automata|Graph")
	float GetWindowYMax() const { return float(CachedYMax); }

	/** Текущее дно по Y - для подписи оси. Ноль у обычного графика населения;
	 *  у нормированного дно поднято, иначе узкая полоса значений выглядит
	 *  прямой линией (см. GenerationHistory::ComputeNiceRange()). */
	UFUNCTION(BlueprintPure, Category = "Automata|Graph")
	float GetWindowYMin() const { return float(CachedYMin); }

	/** Показатель роста, измеренный по текущему окну (наклон log P по log n).
	 *  Считается ВСЕГДА, включена нормировка или нет: это самостоятельный
	 *  результат прогона, а не служебное число графика.
	 *
	 *  Читается как размерность структуры, пока фронт расползается с постоянной
	 *  скоростью: 3 - сплошное тело, около 2 - оболочка или плоская фигура,
	 *  между ними - фрактал. 0 означает "измерить не по чему" (окно короче двух
	 *  замеров или сетка вымерла), а не нулевой рост. */
	UFUNCTION(BlueprintPure, Category = "Automata|Graph")
	float GetFittedGrowthExponent() const { return float(CachedFittedExponent); }

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

	/** Сглаживает отрезок оси Y: сырые границы, пересчитываемые каждый кадр,
	 *  заставляют всю кривую непрерывно дышать.
	 *
	 *  Два режима, и они несводимы друг к другу. Обычный график населения -
	 *  дно в нуле (иначе кривая врёт о том, во сколько раз что-то выросло),
	 *  потолок округляется до "красивого" (1/2/5 * 10^k), растёт немедленно, а
	 *  падает только когда сырой максимум ушёл ниже половины текущего:
	 *  гистерезис, иначе на границе округления масштаб мигал бы туда-сюда
	 *  каждый кадр. Нормированный - отрезок по обеим границам окна
	 *  (GenerationHistory::ComputeNiceRange()), потому что всё значение там
	 *  лежит в узкой полосе, а от нуля она выглядит прямой линией. */
	void UpdateYRange(double RawMinY, double RawMaxY);

	/** Показатель, который реально уходит в нормировку: 0, когда она выключена
	 *  (в этом виде его понимает вся математика в GenerationHistory). */
	double GetActiveExponent() const;

	/** Синтетическая кривая для Designer: без PIE оркестратора не существует,
	 *  и виджет был бы пустым прямоугольником, который нечем мерить. */
	static void BuildDesignTimePoints(const FVector2f& PlotSize, const FVector2f& PlotOrigin,
		TArray<FVector2f>& OutAlivePoints, TArray<FVector2f>& OutRenderedPoints);

	UPROPERTY(Transient)
	TObjectPtr<AAutomataOrchestrator> CachedOrchestrator = nullptr;

	double CachedYMin = 0.0;
	double CachedYMax = 1.0;
	double CachedFittedExponent = 0.0;
	int64 CachedMinGeneration = 0;
	int64 CachedMaxGeneration = 0;

	TSharedPtr<SGenerationGraph> MyGraph;
};
