#pragma once

#include "CoreMinimal.h"
#include "RenderPresets.generated.h"

/** Один готовый профиль качества картинки (см.
 *  AAutomataOrchestrator::GetRenderPresets()/ApplyRenderPreset(), хоткеи F1-F4).
 *
 *  USTRUCT(BlueprintType) с BlueprintReadOnly-полями - тот же идиом, что
 *  FRulePreset/сводок HUD: таблицу читает UMG-виджет, а не только нативный код,
 *  и менять её из Blueprint нельзя (пресеты - константы кода, не состояние
 *  актора).
 *
 *  Зачем это отдельно от россыпи хоткеев Z/X/V/B/C/H: те переключают ПО ОДНОЙ
 *  настройке, и чтобы "сделать быстро", их надо было нажать полдюжины подряд, в
 *  правильном порядке и помня, что именно ты выключил в прошлый раз. Пресет
 *  выставляет весь набор разом и, главное, ДЕТЕРМИНИРОВАННО: каждый пресет
 *  задаёт значение КАЖДОГО поля и каждого cvar'а из общего списка (см.
 *  ConsoleCommands), поэтому переключение между пресетами никогда не оставляет
 *  хвостов от предыдущего - восстанавливать ничего не нужно, следующий пресет
 *  перезаписывает всё. Именно поэтому здесь нет "не трогать это поле": пресет,
 *  который что-то не задаёт, зависел бы от того, откуда в него пришли. */
USTRUCT(BlueprintType)
struct FRenderPreset
{
	GENERATED_BODY()

	/** Отображаемое имя - оно же уходит в HUD (FHudRenderStats::RenderPresetName). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	FString Name;

	/** Короткое пояснение "что это даёт" - для подсказки в HUD. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	FString Description;

	/** VIEWMODE LIT (true) или VIEWMODE UNLIT (false) - ровно то, что раньше
	 *  делали отдельные хоткеи Lit/Unlit, теперь часть профиля. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	bool bLit = true;

	/** Видно ли фон (небо/туман) - см.
	 *  AAutomataOrchestrator::SetBackgroundVisible(). Свет при выключении
	 *  сохраняется, это не то же самое, что погасить источники. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	bool bShowBackground = true;

	/** Отбрасывают ли клетки тени (CastShadow на ISM/HISM-компонентах).
	 *  Самая дорогая позиция из всех: shadow pass прогоняет те же миллионы
	 *  инстансов ещё раз на каждый теневой каскад. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	bool bCellsCastShadows = true;

	/** Отсечение инстансов по расстоянию (AAutomataOrchestrator::
	 *  bEnableCellCulling, хоткей B). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	bool bCellCullingEnabled = false;

	/** Дистанции для отсечения выше. Пресет задаёт их явно, а не оставляет
	 *  подобранные вручную: значение "включить отсечение, но не сказать, с
	 *  какого расстояния" зависело бы от того, что крутили до этого, а профиль
	 *  должен давать один и тот же результат независимо от предыстории.
	 *  Подобранные вручную числа при этом теряются - если нужно своё,
	 *  выставляйте его после пресета (B оставляет числа нетронутыми, см.
	 *  bEnableCellCulling). */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	float CellCullStartDistance = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	float CellCullEndDistance = 0.0f;

	/** Ghost Shape - силуэт по чанкам вместо клеток (см.
	 *  AAutomataOrchestrator::bEnableGhostShape, хоткей H). Без куба отсечения
	 *  он ЗАМЕНЯЕТ детальный рендер целиком - это и есть главный выигрыш
	 *  профиля Performance, а не косметика. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	bool bGhostShapeEnabled = false;

	/** Значения движковых cvar'ов, по одной команде на строку ("r.Xxx 0").
	 *  У всех пресетов список одинаковой длины и с одинаковыми именами -
	 *  различаются только значения (см. doc-comment структуры про
	 *  детерминированность).
	 *
	 *  Чего здесь намеренно НЕТ: r.Nanite. Отключение Nanite на меше клетки в
	 *  этом проекте уже пробовали - рендер стал ХУЖЕ, плюс полезли отдельные
	 *  артефакты (см. CLAUDE.md, раздел про CellCullStartDistance). Повторять
	 *  этот замер не нужно. */
	UPROPERTY(BlueprintReadOnly, Category = "Automata|Rendering")
	TArray<FString> ConsoleCommands;
};

/** Таблица профилей рендера - плайн-namespace, как RulePresets/CellAging/
 *  CellSelection (не UObject: константные данные, ничего не хранит и не
 *  мутирует). */
namespace RenderPresets
{
	/** Все профили в порядке отображения; индекс в этом массиве - то, что
	 *  принимает ApplyRenderPreset() и что вешается на F1-F4. Ссылка на
	 *  статическую таблицу, строится один раз при первом обращении. */
	CELLULARAUTOMATA_API const TArray<FRenderPreset>& GetAll();

	/** Индекс профиля съёмки - обычного или экономного по видеопамяти. Поиск по
	 *  имени, а не "последний в таблице": профилей съёмки два, и позиционный
	 *  расчёт сломался бы от любой вставки. INDEX_NONE, если такого профиля в
	 *  таблице нет - вызывающий обязан проверить и отказаться, а не снимать
	 *  чем попало. */
	CELLULARAUTOMATA_API int32 GetPhotoPresetIndex(bool bLeanMemory);
}