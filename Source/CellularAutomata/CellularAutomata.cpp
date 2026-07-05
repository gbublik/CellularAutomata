// Copyright Epic Games, Inc. All Rights Reserved.

#include "CellularAutomata.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

void FCellularAutomataModule::StartupModule()
{
	// Виртуальный путь /Project/CellularAutomata сопоставляется с
	// Source/CellularAutomata/Shaders - FCellularAutomatonStepCS ссылается
	// на него в IMPLEMENT_GLOBAL_SHADER (см. GpuComputeStrategy.cpp).
	AddShaderSourceDirectoryMapping(TEXT("/Project/CellularAutomata"), FPaths::Combine(FPaths::ProjectDir(), TEXT("Source/CellularAutomata/Shaders")));
}

IMPLEMENT_PRIMARY_GAME_MODULE( FCellularAutomataModule, CellularAutomata, "CellularAutomata" );
