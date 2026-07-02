#pragma once

#include "CoreMinimal.h"

class FCellGrid;

/** Абстрактный рендерер сетки клеток. НЕ владеет Grid - он передаётся
 *  параметром в каждый Render(), что развязывает время жизни рендерера
 *  и время жизни конкретной сетки (нужно для перерисовки после каждого
 *  шага симуляции). */
class CELLULARAUTOMATA_API FCellGridRenderer
{
public:
	virtual ~FCellGridRenderer() = default;

	FCellGridRenderer(const FCellGridRenderer&) = delete;
	FCellGridRenderer& operator=(const FCellGridRenderer&) = delete;

	virtual void Render(const FCellGrid& Grid) = 0;

protected:
	FCellGridRenderer() = default;
};
