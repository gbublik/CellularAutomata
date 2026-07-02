# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Unreal Engine 5.7 C++ game project (`CellularAutomata.uproject`) implementing a 3D cellular-automata simulation, rendered with plain `UInstancedStaticMeshComponent` cell instances (one cube mesh instanced per live cell). The gameplay module is `CellularAutomata` (Runtime, Default loading phase).

The project briefly depended on the third-party **VoxelFree** plugin (SDF/marching-cubes voxel terrain engine) but that was dropped and the plugin fully removed — it was built for volumetric terrain, not for placing discrete cell instances, and was unnecessary overhead for this project's needs. Do not reintroduce a `Voxel` module dependency without deliberate reconsideration.

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
- **`AGamePlayerController`** (`Core/PlayerController/GamePlayerController.*`) — owns the camera. No spline-following camera anymore (removed — see below); just toggles between gameplay input (`FInputModeGameOnly`, pawn input enabled) and UI input (`FInputModeGameAndUI`, cursor shown, pawn input disabled) via `SetCameraControlEnabled()`, and on `BeginPlay` sets FOV and forces `VIEWMODE UNLIT` via console command (matches the editor's "Без освещения" viewport mode — skips lighting cost when rendering large numbers of instanced automaton cells).
- **`AAutomataOrchestrator`** (`Orchestration/AutomataOrchestrator.*`) — the top-level conductor, placed in the level as an Actor. Root component is the `CellsMesh` (`UInstancedStaticMeshComponent`) itself — there is no separate scene-root or camera spline (that system was fully removed; `AGamePlayerController` no longer has `StartSplineMovement`/`StopSplineMovement`/spline-following `Tick`). Owns simulation parameters (`GridSize`, `Amount`, `Seed`, `SpawnRadius`, `ClusterFactor`, `Speed`, `CellMesh`/`CellMaterial`/`CellSize`) exposed as `CallInEditor` UFUNCTIONs (`Start/Pause/Resume/Stop/Next/Clear/GenerateRandom/NewSeed`) so designers can drive the sim from the Details panel. `BeginPlay` wires up the player controller and immediately calls `GenerateRandom()` — the HUD is **not** shown automatically anymore (`InitializeHUD()`/`FUiController` still exist and are called from `PostActorCreated()` for editor-time HUD class validation, but no longer from `BeginPlay`). `NewSeed()` re-rolls `Seed` via `FMath::Rand()` and calls `GenerateRandom()`. **Still stubs**: `Next`, `Clear`, `Stop` — the actual step/tick automaton logic is not yet wired up. Cell storage/rendering is delegated to the `Automata/` subsystem (below), not implemented inline.
- **`Automata/Grid/`** — `FCellGrid` (`CellGrid.h`, abstract, plain C++ not `UObject`): stores live cells addressed by integer `FIntVector` coordinates, decoupled from rendering. `GetAliveCells`/`SetAlive`/`Clear`/`Num` are pure virtual; `GridToWorld(FIntVector)` is **virtual with a default cubic-lattice implementation** (`Cell * CellSize`) — deliberately virtual so a future non-cubic topology (e.g. hex) can override just the coordinate-to-world math without touching storage or rendering. `FSparseCellGrid` (`SparseCellGrid.*`) is the only concrete implementation so far: `TSet<FIntVector>`-backed, good for large mostly-empty grids. A future dense (flat-array) implementation would live alongside it.
- **`Automata/Rendering/`** — `FCellGridRenderer` (`CellGridRenderer.h`, abstract, plain C++): `Render(const FCellGrid&)`, grid passed per-call rather than owned, so one renderer can redraw after each simulation step. `FInstancedMeshCellGridRenderer` (`InstancedMeshCellGridRenderer.*`) is the only concrete implementation: wraps a `UInstancedStaticMeshComponent*` (owned by the Actor, not this class), with `SetMesh`/`SetMaterial` setters called fresh before each `Render()` since those are designer-editable properties. Scales each instance so the mesh's actual bounding-box size matches `CellSize` (via `UStaticMesh::GetBounds()`), so grid cells tile evenly regardless of the assigned mesh's native dimensions. `AAutomataOrchestrator` holds this via the **concrete** type `TUniquePtr<FInstancedMeshCellGridRenderer>` (not the abstract base) since it's the only renderer that exists and needs mesh/material setters the abstract interface deliberately doesn't expose.
- **`FUiController`** (`Ui/UiController.*`) — plain (non-UObject) RAII-style helper, not an actor. Lazily creates a `UUserWidget` from a `TSubclassOf<UUserWidget>` set by the orchestrator, and exposes `ShowHUD/HideHUD/ToggleHUD`. Owned via `TUniquePtr` by `AAutomataOrchestrator`.

All of the plain-C++ helper classes above (`FUiController`, `FCellGrid`/`FSparseCellGrid`, `FCellGridRenderer`/`FInstancedMeshCellGridRenderer`) follow the same convention: non-copyable, owned via `TUniquePtr` by whichever `UObject`/Actor uses them, `TWeakObjectPtr` (not raw pointers) for any `UObject` references they hold.

### Source layout convention

Code is being migrated from a flat `Orchestration/` and `Ui/` dump into a `Core/<Subsystem>/` layout (see the pending rename of `AGameMode`/`GamePlayerController` in git status). When adding new engine-level classes (game mode, controllers, pawns, etc.), place them under `Source/CellularAutomata/{Public,Private}/Core/<Subsystem>/`; higher-level gameplay/orchestration code stays under `Orchestration/`; widget/HUD glue stays under `Ui/`.

### Module dependencies

`CellularAutomata.Build.cs` pulls in `Core, CoreUObject, Engine, InputCore, Slate, SlateCore, UMG, WebBrowser, WebBrowserWidget, Json, JsonUtilities`. The `WebBrowser`/`WebBrowserWidget` dependency was for a `Ui/WebInterface` widget that has since been deleted — if reintroducing browser-based UI, that's the intended integration point.

`RemoteControlWebInterface` and `RiderLink` are both present but **disabled** (`"Enabled": false`) in the `.uproject` Plugins list — `RemoteControlWebInterface` tries to build a React web app on editor startup and fails in this environment (`Failed To Build WebApp`); `RiderLink`'s `RiderDebuggerSupport.dll` gets blocked by Windows Smart App Control (unsigned binary, `GetLastError=4551`). Don't re-enable either without addressing the underlying cause first.

### MCP / AI-editor integration

`Plugins/McpAutomationBridge` (from github.com/ChiR24/Unreal_mcp) is installed and gives Claude Code direct control of the running Editor over HTTP — spawning/inspecting actors, screenshots, console commands, asset ops, etc. Native MCP mode is enabled via `Config/DefaultGame.ini` (`[/Script/McpAutomationBridge.McpAutomationBridgeSettings]`, `bEnableNativeMCP=True`, port 3000) — no Node.js/bridge process needed. Registered in Claude Code at **user scope** (`claude mcp add unreal-engine --transport http --scope user http://localhost:3000/mcp`); MCP server tool lists only refresh when a Claude Code session (re)starts, not mid-session.

**Fast iteration loop while the Editor is already open**: after editing C++, trigger a hot patch instead of closing the editor for a full UBT rebuild — via the MCP `control_editor` tool's `console_command` action (or the in-editor Ctrl+Alt+F11 hotkey) running `LiveCoding.Compile`. This requires Live Coding to be enabled in the Editor already. A pure function-body edit (no new/changed `UPROPERTY`/`UFUNCTION`) patches in under a second; if unsure whether a patch actually landed, compare the edited `.cpp`'s mtime against `Intermediate/Build/Win64/x64/UnrealEditor/Development/CellularAutomata/<File>.cpp.obj` and `Binaries/Win64/UnrealEditor-CellularAutomata.dll`. A full cold UBT build still requires the Editor process to be closed first (it locks the DLL):
`"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" CellularAutomataEditor Win64 Development -Project="<repo>\CellularAutomata.uproject" -WaitMutex`

### Config

- `Config/DefaultEngine.ini`: `GameDefaultMap=/Game/levels/NewMap.NewMap`, `GlobalDefaultGameMode=/Script/CellularAutomata.AGameMode` (i.e. `AAGameMode`).
- `Config/DefaultInput.ini`: input action/axis bindings consumed by `AGamePlayerController`.
