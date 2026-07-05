// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/** Кастомный модуль вместо стандартного FDefaultGameModuleImpl - нужен
 *  StartupModule() для регистрации виртуального пути шейдеров проекта
 *  (см. Shaders/Private/CellularAutomatonStep.usf,
 *  Automata/Simulation/ComputeStrategy/GpuComputeStrategy.cpp). */
class FCellularAutomataModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override;
};

