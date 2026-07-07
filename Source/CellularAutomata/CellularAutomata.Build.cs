// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CellularAutomata : ModuleRules
{
	public CellularAutomata(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore",
			"UMG",
			"WebBrowser",        // Для UWebBrowser
			"WebBrowserWidget",  // Для UWebBrowser в UMG
			"Json",
			"JsonUtilities"
		});
		// RHI/RenderCore - для FGpuComputeStrategy (RDG compute shader dispatch,
		// см. Automata/Simulation/ComputeStrategy/GpuComputeStrategy.cpp).
		// ProceduralMeshComponent - для BakeCellsToMesh() (запекание клеток в
		// цельный меш, см. Automata/Meshing/CellMeshBuilder и
		// AAutomataOrchestrator::BakedMeshComponent).
		// DesktopPlatform - системные диалоги выбора файла для
		// SaveStateToFile()/LoadStateFromFile(); Developer-модуль, в Shipping
		// недоступен (проект живёт в editor/PIE, этого достаточно).
		PrivateDependencyModuleNames.AddRange(new string[] { "EnhancedInput", "RHI", "RenderCore", "ProceduralMeshComponent", "DesktopPlatform" });

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
