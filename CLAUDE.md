# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Unreal Engine 5.7 C++ game project (`CellularAutomata.uproject`) implementing a 3D cellular-automata simulation, presumably running on a voxel grid via the bundled **VoxelFree** plugin (`Plugins/VoxelFree`, marketplace free tier, source in-tree). The gameplay module is `CellularAutomata` (Runtime, Default loading phase).

## Build / run

There is no CLI test suite or lint config in this repo — it's a standard UE C++ project built through the Unreal toolchain.

- Engine install: `C:\Program Files\Epic Games\UE_5.7`
- Regenerate Visual Studio project files after adding/removing source files:
  `"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" -projectfiles -project="<repo>\CellularAutomata.uproject" -game -rocket -progress`
- Compile from the command line (Editor target, Development config, Win64):
  `"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" CellularAutomataEditor Win64 Development -Project="<repo>\CellularAutomata.uproject"`
- Normal workflow is opening `CellularAutomata.sln` in Visual Studio (or the `.uproject` in Rider) and building/running from there, or launching the Editor directly via the `.uproject` file.
- Two build targets exist: `CellularAutomataTarget` (Game) and `CellularAutomataEditorTarget` (Editor) — see `Source/CellularAutomata*.Target.cs`.

## Architecture

Actor/controller composition, not a monolithic GameMode:

- **`AAGameMode`** (`Core/GameMode/AGameMode.*`) — minimal; only wires `PlayerControllerClass` to `AGamePlayerController`. Note the doubled `A` prefix: the UE Actor-class convention (`A`) plus the class's own name (`AGameMode`) yields `AAGameMode`. `DefaultEngine.ini`'s `GlobalDefaultGameMode` points at this class.
- **`AGamePlayerController`** (`Core/PlayerController/GamePlayerController.*`) — owns the camera. Two responsibilities live here:
  - Toggling between gameplay input (`FInputModeGameOnly`, pawn input enabled) and UI input (`FInputModeGameAndUI`, cursor shown, pawn input disabled) via `SetCameraControlEnabled()`.
  - Driving the camera along a `USplineComponent` over time (`StartSplineMovement` / `StopSplineMovement` / `UpdateSplineMovement`, ticked every frame), looking at a fixed `LookAtTarget`.
- **`AAutomataOrchestrator`** (`Orchestration/AutomataOrchestrator.*`) — the top-level conductor, placed in the level as an Actor (root component is the camera spline itself). Owns simulation parameters (`GridSize`, `Amount`, `Seed`, `ClusterFactor`, `Speed`) exposed as `CallInEditor` UFUNCTIONs (`Start/Pause/Resume/Stop/Next/Clear/GenerateRandom/NewSeed`) so designers can drive the sim from the Details panel. On `BeginPlay`/`PostActorCreated` it looks up the first `PlayerController`, casts it to `AGamePlayerController`, hands it the camera spline, and creates the HUD via `FUiController`. **Most of the simulation-control functions are currently stubs** (`Next`, `Clear`, `GenerateRandom`, `Stop`, `NewSeed` have empty bodies) — the actual cellular-automata/voxel logic is not yet wired up here.
- **`FUiController`** (`Ui/UiController.*`) — plain (non-UObject) RAII-style helper, not an actor. Lazily creates a `UUserWidget` from a `TSubclassOf<UUserWidget>` set by the orchestrator, and exposes `ShowHUD/HideHUD/ToggleHUD`. Owned via `TUniquePtr` by `AAutomataOrchestrator`.

### Source layout convention

Code is being migrated from a flat `Orchestration/` and `Ui/` dump into a `Core/<Subsystem>/` layout (see the pending rename of `AGameMode`/`GamePlayerController` in git status). When adding new engine-level classes (game mode, controllers, pawns, etc.), place them under `Source/CellularAutomata/{Public,Private}/Core/<Subsystem>/`; higher-level gameplay/orchestration code stays under `Orchestration/`; widget/HUD glue stays under `Ui/`.

### Module dependencies

`CellularAutomata.Build.cs` pulls in `Core, CoreUObject, Engine, InputCore, Slate, SlateCore, UMG, WebBrowser, WebBrowserWidget, Json, JsonUtilities, Voxel`. The `WebBrowser`/`WebBrowserWidget` dependency was for a `Ui/WebInterface` widget that has since been deleted — if reintroducing browser-based UI, that's the intended integration point. `RemoteControlWebInterface` is also enabled as a plugin in the `.uproject`.

### Config

- `Config/DefaultEngine.ini`: `GameDefaultMap=/Game/levels/NewMap.NewMap`, `GlobalDefaultGameMode=/Script/CellularAutomata.AGameMode` (i.e. `AAGameMode`).
- `Config/DefaultInput.ini`: input action/axis bindings consumed by `AGamePlayerController`.
