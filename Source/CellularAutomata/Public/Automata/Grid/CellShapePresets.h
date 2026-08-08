#pragma once

#include "CoreMinimal.h"
#include "Automata/Generation/CellParityFilter.h"
#include "Automata/Grid/CellShape.h"
#include "Automata/Simulation/Neighborhood.h"
#include "Automata/Simulation/LatticeNeighborhood.h"
#include "CellShapePresets.generated.h"

/** Одна из пяти форм клетки, замощающих пространство параллельными
 *  переносами, - готовый набор согласованных настроек (см.
 *  AAutomataOrchestrator::GetCellShapePresets()/ApplyCellShapePreset()).
 *
 *  ПОЧЕМУ ПРЕСЕТ, А НЕ ОТДЕЛЬНОЕ ПОЛЕ "ФОРМА". Форма клетки - не независимая
 *  настройка, а СЛЕДСТВИЕ четырёх других: какие узлы решётки заселены
 *  (фильтр чётности), какие считаются соседями, как решётка растянута и
 *  насколько меш крупнее шага. Держать её отдельным состоянием значило бы
 *  завести пятый источник правды, который спорит с четырьмя остальными -
 *  выбрал "ромбододекаэдр", а фильтр чётности остался от прошлого раза, и
 *  картинка не соответствует ни одному из двух. Пресет вместо этого
 *  выставляет все четыре разом и уходит; поля остаются обычными,
 *  редактируемыми, и любая их комбинация по-прежнему законна - в том числе
 *  заведомо "неправильная", на которой стоит встречная проверка теста
 *  Generation.ParityFilter (Moore на ГЦК обязан ломать чётность).
 *
 *  Тот же идиом, что FRulePreset/FRenderPreset/FStateGeneratorPreset:
 *  константная таблица в коде, BlueprintReadOnly-поля, применение через одну
 *  функцию.
 *
 *  ЧТО ПРЕСЕТ НЕ ТРОГАЕТ: правило (Birth/Survival) и генератор начального
 *  состояния. Форма - это геометрия, правило - динамика; менять их вместе
 *  значило бы лишить возможности посмотреть одно и то же правило на разных
 *  решётках, ради чего всё и делалось. */
USTRUCT(BlueprintType)
struct FCellShapePreset
{
	GENERATED_BODY()

	/** Устойчивое имя формы - им адресуются тумблер в Details panel, слот меша
	 *  и FRulePreset::RequiredCellShape. Индекс в таблице для этого не годится:
	 *  он меняется от перестановки строк. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	ECellShape Shape = ECellShape::Cube;

	/** Отображаемое имя фигуры. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	FString Name;

	/** Решётка, на которой эта форма возникает, и чем она интересна. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	FString Description;

	/** Сколько граней у ячейки Вороного - оно же число соседей, с которыми
	 *  клетка реально соприкасается. Ровно одна грань на соседа: это не
	 *  совпадение, а определение ячейки Вороного, и именно поэтому форму
	 *  нельзя выбрать в отрыве от соседства. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	int32 FaceCount = 0;

	/** Какие узлы Z^3 заселены - см. ECellParityFilter. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	ECellParityFilter ParityFilter = ECellParityFilter::None;

	/** Соседство, у которого столько же элементов, сколько у формы граней. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	ENeighborhood Neighborhood = ENeighborhood::VonNeumann;

	/** Набор соседей, не выражаемый оболочками, - если форме нужен именно
	 *  такой (см. ELatticeNeighborhood). Shells значит "хватает поля
	 *  Neighborhood выше". */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	ELatticeNeighborhood NeighborhoodShape = ELatticeNeighborhood::Shells;

	/** Растяжение решётки по Z - см. AAutomataOrchestrator::LatticeZScale. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	float LatticeZScale = 1.0f;

	/** Во сколько раз меш крупнее шага решётки по X - см.
	 *  AAutomataOrchestrator::CellMeshScaleMultiplier. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	float CellMeshScaleMultiplier = 1.0f;

	/** Габарит подходящего меша (его bounding box) в единицах, где шаг
	 *  решётки в плоскости равен 1. Движок пропорции меша не проверяет никак,
	 *  а неверный меш даёт щели или наложение - то есть выглядит ровно как
	 *  неверно выбранная решётка. Поэтому ApplyCellShapePreset() сверяет
	 *  назначенный CellMesh с этим числом и говорит вслух, если они
	 *  разошлись. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Cells")
	FVector ExpectedMeshAabb = FVector::OneVector;
};

/** Таблица форм клетки - плайн-namespace, как RulePresets/RenderPresets. */
namespace CellShapePresets
{
	/** Все формы в порядке отображения. Ссылка на статическую таблицу,
	 *  строится один раз при первом обращении. */
	CELLULARAUTOMATA_API const TArray<FCellShapePreset>& GetAll();

	/** Место формы в GetAll() - единственный переход от устойчивого имени к
	 *  индексу, которым таблица адресуется (ApplyCellShapePreset(), консольная
	 *  команда CA.CellShape, выпадашка в HUD). INDEX_NONE не бывает, пока
	 *  таблица покрывает всё перечисление, и ровно это проверяет тест
	 *  CellShape.PresetTableCoversEnum. */
	CELLULARAUTOMATA_API int32 IndexOf(ECellShape Shape);

	/** Правда, если форма живёт на скошенной решётке, которой FLatticeTransform
	 *  пока не умеет (сейчас это только гексагональная призма).
	 *
	 *  Признак выводится из самого пресета - ожидаемый меш шире по Y, чем по X,
	 *  а прямоугольная решётка такого дать не может, - а не заводится отдельным
	 *  булем: реализация скошенного отображения снимет ограничение сама, не
	 *  требуя не забыть снять флаг. Отдельной функцией, потому что спрашивают
	 *  об этом два места (применение формы и обратный поиск формы по полям), и
	 *  разъехавшиеся копии условия означали бы, что одно из них применяет
	 *  форму, которую другое считает неподдержанной. */
	CELLULARAUTOMATA_API bool RequiresShearedLattice(const FCellShapePreset& Preset);
}
