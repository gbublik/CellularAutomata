#pragma once

#include "CoreMinimal.h"
#include "Automata/Rendering/CellRenderInstance.h"

class FCellGrid;

/** Абстрактный рендерер сетки клеток. НЕ владеет Grid - он передаётся
 *  параметром в каждый Render(), что развязывает время жизни рендерера
 *  и время жизни конкретной сетки (нужно для перерисовки после каждого
 *  шага симуляции).
 *
 *  ЧТО рисовать задаётся не сеткой, а явным списком FCellRenderInstance
 *  (позиция + цвет): цвет клетки зависит от её возраста/стадии угасания, а
 *  это политика оркестратора, а не рендерера - и заодно позволяет рисовать
 *  подмножество (подсветка выделения, отсечение кубом) без обёрток над
 *  самой сеткой. Grid остаётся в сигнатуре ради GetCellSize() - рендереру
 *  нужно подогнать масштаб меша под размер клетки. */
class CELLULARAUTOMATA_API FCellGridRenderer
{
public:
	virtual ~FCellGridRenderer() = default;

	FCellGridRenderer(const FCellGridRenderer&) = delete;
	FCellGridRenderer& operator=(const FCellGridRenderer&) = delete;

	virtual void Render(const FCellGrid& Grid, TArray<FCellRenderInstance>&& Instances) = 0;

protected:
	FCellGridRenderer() = default;
};
