# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**It is a map, not the whole territory.** Each subsystem gets a summary here and a file under `docs/` holding the full reasoning — the measured numbers, the engine traps, and the approaches that were tried and rejected. Read the linked file *before* changing that subsystem; the rejected-approach notes are the most valuable part of this documentation and exist precisely so a dead end is not walked twice. When a change lands, update the `docs/` file it belongs to, and this one only if the summary itself stopped being true.

## Project overview

Unreal Engine 5.7 C++ game project (`CellularAutomata.uproject`) implementing a 3D cellular-automata simulation, rendered with `UHierarchicalInstancedStaticMeshComponent` cell instances (one cube mesh instanced per live cell). The gameplay module is `CellularAutomata` (Runtime, Default loading phase).

The project briefly depended on the third-party **VoxelFree** plugin (SDF/marching-cubes voxel terrain engine) but that was dropped and the plugin fully removed — it was built for volumetric terrain, not for placing discrete cell instances, and was unnecessary overhead for this project's needs. Do not reintroduce a `Voxel` module dependency without deliberate reconsideration.

## Build / run

No lint config — it's a standard UE C++ project built through the Unreal toolchain. There *is* a small automation test suite (`Source/CellularAutomata/Private/Tests/AutomataTests.cpp`, whole file under `#if WITH_DEV_AUTOMATION_TESTS`, no extra `Build.cs` dependency), run headless with:

`"C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<repo>\CellularAutomata.uproject" -ExecCmds="Automation RunTests CellularAutomata" -testexit="Automation Test Queue Empty" -unattended -nopause -nosplash -abslog=<file>`

Twenty-four tests, deliberately covering only what needs no actor, tick or render — that is where UE tests get expensive and brittle, and it is checked by hand in PIE instead. What each one guards, and why it exists, is in **`docs/testing.md`**. Note the whole run costs an editor startup (tens of seconds); there is no fast inner loop for UE automation tests.

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

- **`AGamePlayerController`** (`Core/PlayerController/GamePlayerController.*`) — owns the camera and every hotkey in the project (roughly forty: play/step/reset, speed and `StepsPerRender`, render profiles, culling, selection, capture, save/load, and the whole NumPad). Built on Enhanced Input, **except** Space/R/N/Y, the digits and the NumPad, which are caught in a raw `InputKey()` override: Enhanced Input samples a key's down/up *level* once per `Tick`, so a quick press inside one long lagged frame is never seen as a transition — and those keys are pressed precisely when the sim lags hardest. Ctrl/Shift combinations are always checked *inside* the handler, never mapped, because Enhanced Input cannot require a modifier in a key mapping. → **`docs/input-and-camera.md`**

- **`AGameCameraManager`** (`Core/CameraManager/GameCameraManager.*`) — adds exactly one thing to `APlayerCameraManager`: a switchable **orthographic** projection, which together with the axial NumPad views is how a DCC viewport is used to examine a shape. Projection lives in an `FMinimalViewInfo` the manager rebuilds every frame, so `UpdateViewTarget()` is the only hook that works. → **`docs/input-and-camera.md`**

- **`AGameHud`** (`Core/Hud/GameHud.*`, wired via `AAGameMode::HUDClass`) — a native `AHUD` whose `DrawHUD()` paints the live marquee rectangle during a selection drag. Unrelated to the UMG HUD below.

- **`AAutomataOrchestrator`** (`Orchestration/AutomataOrchestrator.*`) — the top-level conductor, placed in the level as an Actor. Owns the grid, the renderer, the simulation parameters (exposed as `CallInEditor` UFUNCTIONs so a designer drives the sim from the Details panel) and the HUD. Three things shape everything else about it: the simulation step runs **off the game thread** (`StepAsync()`), the render can be **spread across frames** (chunked), and generations can be computed **ahead of the display** (`StepsPerRender`) — so several paths race for one `Grid` pointer, and every path that swaps it guards on `bStepInProgress`. **One class in one header, but seventeen `.cpp` files** (`AutomataOrchestrator.cpp` plus `_Stepping`, `_Rendering`, `_Culling`, `_Editing`, `_Painting`, `_Capture`, `_Persistence`, `_Selection`, `_Rules`, `_Hud`, `_Generation`, `_Photo`, `_Baking`, `_Bounds`, `_Sonification`, `_GhostShape`) — the header is left whole because two thirds of it is the documentation that makes the class readable. The split is physical only: UHT reads the header, so no reflection name, Blueprint binding or `.umap` override depends on which file a method sits in — and correspondingly it fixes nothing about coupling or Live Coding. → **`docs/orchestrator.md`**

- **`Automata/Grid/`** — `FCellGrid` (abstract, plain C++) stores live cells by integer `FIntVector`, decoupled from rendering; `FDenseCellGrid` is the sole implementation — fixed-size cubic chunks of packed `TBitArray` in a sparse `TMap`, created lazily and pruned when empty, with parallel byte arrays for per-cell age and decay state. Chunk math needs **real floor division** (`FMath::DivideAndRoundDown` truncates toward zero, which is not the same thing): generation is centred on the origin, so negative coordinates are the norm, not an edge case. → **`docs/grid-and-simulation.md`**

- **`Automata/Grid/LatticeTransform.h`** + **`CellShapePresets.*`** — how integer cell coordinates become world positions, and the five convex polyhedra that tile space by translation (Fedorov, 1885 — a **closed** classification, so the preset table has a final length). `FLatticeTransform` is a POD, deliberately not the project's usual abstract-class-behind-`TUniquePtr`, because it is called millions of times per frame rather than once per step. `FCellGrid::CellSize` was **deleted rather than supplemented** when it arrived: the inverse mapping did not exist and four hand-written divisions stood in its place, so deleting the field turned each into a compile error. Four of the five shapes work; the hexagonal prism needs a sheared mapping the type cannot yet express. → **`docs/lattice-and-cell-shapes.md`**

- **`Automata/Simulation/`** — the rule and how a generation is computed. `ENeighborhood` is 14 values built from **four shells** within radius 2 (faces, edges, corners, far axes); there is no separate radius, reach is derived from the offsets. Alongside it: `FCellularAutomatonRule`, the `Survival/Birth/States/Neighborhood` rule-string parser, `RulePresets`, and Generations multi-state decay (`States > 2`, costing nothing at the default 2). → **`docs/grid-and-simulation.md`**

- **`Automata/Simulation/ComputeStrategy/`** — `FCellularAutomatonComputeStrategy` declares one synchronous contract (`Step()`, plus `StepBatch()` for several generations per invocation); `FCpuComputeStrategy` and `FGpuComputeStrategy` implement it, both grid-storage-agnostic and both reachable from a background thread. This is where the project's performance work lives, and where the measured history matters most — including two documented dead ends. → **`docs/compute-strategies.md`**

- **`Automata/Rendering/`** — one renderer over one instanced component, with per-instance colour from an age/decay ramp. Three *independent* mechanisms decide what reaches the screen, and the terminology is worth keeping straight: **culling** hides instances after they are built (distance-based), while the **cull volume** and the **view slice** remove cells inside `BuildCellRenderData()` before anything is built at all. Also here: render profiles on F1–F4, Ghost Shape, and the F10 photograph. → **`docs/rendering.md`** (renderer, cuts, profiles, photo) and **`docs/cell-color-and-filters.md`** (colour ramps, age filter, per-instance data, measured chunking cost)

- **`Automata/Selection/`** + **`Automata/Meshing/`** — screen-rect marquee and ray-pick (voxel DDA — engine line traces cannot work here, all cell components have collision disabled) cell selection, the `InitialStateCells` return point behind **R**, and the face-culled mesh builder shared by baking and Ghost Shape. → **`docs/selection-and-baking.md`**

- **`Automata/Editing/`** — `CellEditJournal`: the undo journal for **manual** grid edits — deleting selected cells (Delete) and **painting them in** (Shift+Tab draw mode: LMB places a cell against the face you clicked, RMB erases, a ghost follows the cursor; one click, one cell — drag-painting was built and removed). Alongside it `CellClipboard`: Ctrl+C copies the selection, Ctrl+V pastes it under the cursor **pressed against** the face rather than centred on it, as one journal record. It exists because of a split the project makes deliberately — a *generation* is recomputed rather than remembered (**Ctrl+Z** → `StepBackward()`), but a hand edit no rule can reproduce, so it is the one thing stored. Stored as a **delta** (which cells, and their previous alive/age/decay state), never a grid snapshot, and tagged with the generation it happened on — which makes the journal a *script* of the current trajectory, not a pile of states: a rollback re-seeds from `InitialStateCells` and replays the edits at their own generations, so manual edits survive it without a single snapshot being kept. → **`docs/orchestrator.md`**

- **`Automata/Generation/`** — geometric generators of the *initial* state: lattices, solids, noise, symmetric seeds, plus `CellArrayModifier` (Ctrl+D) — Blender's Array modifier over cells, which is filed here but is *not* a generator: it multiplies an existing set (the selection, or the whole grid) rather than building one from nothing. Deliberately **pure geometry** — they never read the rule and never run trial steps; matching a structure to a rule stays outside them, made arithmetic by the neighbour-count histogram (Y / Ctrl+Y) and brute-forceable by `bAutoReseedOnExtinction` (Shift+N), which re-rolls the seed whenever the grid dies and lets the run continue unattended — the orchestrator drives that, the generators still know nothing about the rule. Parity filters turn the cubic lattice into FCC or BCC for free. → **`docs/generation.md`**

- **`Automata/Capture/`** — orthogonal PNG slices rasterised **straight out of `FCellGrid`**: no GPU, no `SceneCapture`, so no antialiasing by construction, no resolution limit, and reproducible regardless of camera. F6 for one slice, F7 for a series. → **`docs/capture.md`**

- **`Automata/Persistence/`** — the `.casave` container: rules plus the *initial* pattern (never the evolved grid) plus a thumbnail. Two independent version fields, two independent jobs. The pattern is written **centred on the origin** (by an even offset, so FCC/BCC seeds keep their sublattice) — only in the file, never in the live grid. → **`docs/persistence.md`**

- **`Automata/Sonification/`** — the simulation is **sonified**, not scored: sound is another instrument alongside the generation graph, and the ear catches periodicity and collapse before the eye does. Everything is measured in `y = ln(1 + AliveCount)` against the **generation number** — that one change of coordinates makes the slope a *relative* growth rate (identical for 100 cells and 7M), keeps a dead grid inside the domain, and survives the holes that `StepsPerRender` and GPU batching punch into the sample series. Curvature is dimensionless on purpose: it scales as the *square* of the slope, so any absolute threshold either always fires or never does. C++ ships parameters and triggers; **the MetaSound graph is built by hand in the editor and is not mine to write** — the same split as the UMG HUD. Events are detected by **edges, with no hooks in the orchestrator at all**. → **`docs/sonification.md`**

- **`Ui/`** — the HUD. C++ supplies data (`FHudStats`, `FCellRenderStats`) and actions as Blueprint API; **the layout is built by hand in UMG and is not mine to write**. A UMG widget never sees a keypress, so anything the HUD must react to needs a `BlueprintImplementableEvent` bridge. → **`docs/hud.md`**

### Conventions that apply everywhere

- **Every `UObject*` member of a `UCLASS` must be a `UPROPERTY`.** Live Coding reinstancing after a hot patch during PIE copies only tagged fields; a plain C++ member survives as *uninitialized garbage*, not `nullptr`, and dereferencing it crashes the whole Editor. This has actually happened here (`GamePC`). The same applies to state that cannot be recomputed — mark it `Transient` so reinstancing preserves or clears it rather than leaving it stale.
- **Anything that swaps or mutates `Grid` guards on `bStepInProgress`** and refuses with a warning, or defers itself through a pending flag (R and N do the latter). A background step is *reading* `*Grid` the whole time it is in flight.
- **Details-panel `UPROPERTY`s are re-read on every call, never cached** — the rule, the compute strategy and the grid are all rebuilt per step, so an edit takes effect immediately. A background thread must never touch them: snapshot on the game thread before dispatch.
- **Logic worth testing lives in a free-function namespace**, not on the actor — `CellAging`, `CellSelection`, `CellMeshBuilder`, `ColorRamp`, `GenerationHistory`, `RuleStringParser`. That is what makes it exercisable without an actor, a tick or a render.
- **A `UENUM` gets its own header** (`ENeighborhood`, `EChunkedRenderOrder`, `ESelectionCombineMode`, `ECellParityFilter`), so the subsystem owns its own types.
- **Preset tables share one shape** (`RulePresets`, `RenderPresets`, `CapturePresets`): a code-constant table, a `USTRUCT(BlueprintType)` with `BlueprintReadOnly` fields, a `GetAll()` namespace function, and a `GetX()`/`ApplyX(Index)` pair on the orchestrator. A preset sets **every** field it owns, so switching can never leave a tail from the previous one.
- **Log and on-screen messages are written in Russian**, matching the rest of the project's user-facing text.
- **UE 5.7 trap**: `Printf`'s format string is `consteval`-checked, so `Printf(bFlag ? TEXT("a %d") : TEXT("b %d"), X)` does not compile at all. Build the string with `if`/`else` instead.

All of the plain-C++ helper classes above (`FUiController`, `FCellGrid`/`FDenseCellGrid`, `FCellGridRenderer`/`FInstancedMeshCellGridRenderer`, `FCellularAutomatonRule`) follow the same convention: non-copyable, owned via `TUniquePtr` (or, for `FCellularAutomatonRule`, constructed as a short-lived local) by whichever `UObject`/Actor uses them, `TWeakObjectPtr` (not raw pointers) for any `UObject` references they hold.

### Source layout convention

Engine-level classes live under `Source/CellularAutomata/{Public,Private}/Core/<Subsystem>/` — that migration is **done** (`Core/GameMode/`, `Core/PlayerController/`, `Core/CameraManager/`, `Core/Hud/`), so put new game modes, controllers and pawns there. Higher-level gameplay/orchestration code stays under `Orchestration/`; widget/HUD glue stays under `Ui/`.

A class whose implementation outgrows one file is split by **responsibility into several `.cpp` files sharing one header**, named `<Class>_<Cluster>.cpp` — see `AAutomataOrchestrator`'s seventeen. This is the cheap move and it should stay cheap: it is invisible to UHT, so it can never break a Blueprint or a level override, and for the same reason it never *fixes* anything either. Reach for it when a file has become hard to navigate or a merge-conflict magnet; do not mistake it for decoupling.

### Module dependencies

`CellularAutomata.Build.cs` pulls in `Core, CoreUObject, Engine, InputCore, Slate, SlateCore, UMG, WebBrowser, WebBrowserWidget, Json, JsonUtilities` as public dependencies, plus `EnhancedInput`, `RHI`, `RenderCore`, `ProceduralMeshComponent`, `DesktopPlatform` as private dependencies. `EnhancedInput` was added for `AGamePlayerController`'s Play/Stop hotkey — `EnhancedInput` is already the project's active input system per `Config/DefaultInput.ini`'s `DefaultPlayerInputClass`/`DefaultInputComponentClass`, so no plugin-enablement change was needed, just the module reference. `RHI`/`RenderCore` were added for `FGpuComputeStrategy`'s RDG compute-shader dispatch (see `Automata/Simulation/ComputeStrategy/`). `ProceduralMeshComponent` is for `BakeCellsToMesh()` (see the Baking paragraph above). `DesktopPlatform` is for the native Save/Open dialogs in `Automata/Persistence/` (see below) — a Developer module, unavailable in Shipping builds, which is fine since this project only runs in editor/PIE; the thumbnail screenshot itself (`FImageUtils`/`FViewport::ReadPixels()`) needed no separate module, already reachable through `Engine`/`RHI`. The `WebBrowser`/`WebBrowserWidget` dependency was for a `Ui/WebInterface` widget that has since been deleted — if reintroducing browser-based UI, that's the intended integration point. **Note**: adding a module to `Build.cs` isn't something Live Coding can hot-patch — it requires closing the Editor and a full cold rebuild (see Build/run section above).

**Audio needs no module, and this is worth not rediscovering.** `Automata/Sonification/` drives MetaSound entirely through `Engine`: `SetFloatParameter`/`SetParameters`/`SetTriggerParameter` are `ENGINE_API` on `ISoundParameterControllerInterface`, `AudioExtensions` already sits in `Engine.Build.cs`'s `PublicIncludePathModuleNames` so its headers come through transitively, and the Metasound plugin is `EnabledByDefault`. Adding `AudioMixer`/`AudioExtensions`/`SignalProcessing` here buys nothing and costs a cold rebuild.

`RemoteControlWebInterface` and `RiderLink` are both present but **disabled** (`"Enabled": false`) in the `.uproject` Plugins list — `RemoteControlWebInterface` tries to build a React web app on editor startup and fails in this environment (`Failed To Build WebApp`); `RiderLink`'s `RiderDebuggerSupport.dll` gets blocked by Windows Smart App Control (unsigned binary, `GetLastError=4551`). Don't re-enable either without addressing the underlying cause first.

### MCP / AI-editor integration

`Plugins/McpAutomationBridge` gives Claude Code direct control of the running Editor over HTTP (native MCP mode, port 3000) — spawning/inspecting actors, screenshots, console commands. Setup, the hot-patch loop and its failure modes are in **`docs/tooling.md`**.

**The one thing to know before hot-patching**: treat a **cold rebuild with the Editor closed as the default** for this project's orchestrator and controller classes. `LiveCoding.Compile` reinstances the *whole* class whenever its `.cpp` is patched, regardless of whether reflection data changed — so even a comments-and-bodies-only edit can kill the Editor process outright when that class has a live instance in the open level. Measured, not assumed; an earlier claim in this file that body-only edits were safe was wrong. Hot-patching is a gamble worth taking only with unsaved work already saved.

### Config

- `Config/DefaultEngine.ini`: `GameDefaultMap=/Game/levels/NewMap.NewMap`, `GlobalDefaultGameMode=/Script/CellularAutomata.AGameMode` (i.e. `AAGameMode`).
- `Config/DefaultInput.ini`: **every hotkey in the project**, as `+ActionMappings` rows named `CA_*` — read by `HotkeyRegistry` and handed to both halves of the input (the Enhanced Input table and the raw `InputKey()` branches), so a key can be rebound in text or through Project Settings → Input without touching code. Defaults live in `HotkeyRegistry::GetDefaults()`, so deleting the block loses the customisation, not the controls. `ActionMappings` is used as a data source only — dispatch stays ours, the legacy binding path is unused. → **`docs/input-and-camera.md`**. Plus a `[/Script/Engine.PlayerInput]` block that **removes five engine `DebugExecBindings`**. `Engine/Config/BaseInput.ini` binds F1–F5 to viewmode commands, and this project owns all five: F1–F4 are render profiles, F5 the HUD info panel. F5 was the visible one — `viewmode shadercomplexity` floods the screen with colour — while F1–F4 collided quietly, both the preset and the engine setting a viewmode with the winner decided by ordering. Removed by exact match rather than clearing the array, so F11 fullscreen, the shader-recompile and profile chords and the debug camera survive; **the strings must match `BaseInput.ini` to the character** (including the space after the comma on F1), because ini array removal compares text rather than the parsed struct and a stray space makes it silently do nothing. Left in place: `LeftShift`/`RightShift` → `DebugManager` column cycling, which overlaps this project's speed boost and every Shift modifier but has not misbehaved.

## Where the detail lives

| file | covers |
|---|---|
| `docs/input-and-camera.md` | every hotkey, the NumPad camera layout, ortho projection, headlight |
| `docs/orchestrator.md` | off-thread stepping, chunked rendering, `StepsPerRender`, the `Grid` races |
| `docs/grid-and-simulation.md` | chunked storage, the four neighbourhood shells, rule strings, Generations decay |
| `docs/lattice-and-cell-shapes.md` | `FLatticeTransform`, the five tiling polyhedra, cell-mesh generation, the border UV |
| `docs/compute-strategies.md` | CPU/GPU strategies, batched dispatch, the optimisation history and its dead ends |
| `docs/rendering.md` | the renderer, the three cuts, render profiles, Ghost Shape, the F10 photograph |
| `docs/cell-color-and-filters.md` | colour ramps and spaces, the age filter, per-instance data, chunking cost |
| `docs/selection-and-baking.md` | marquee and ray-pick selection, `InitialStateCells`, baking to one mesh |
| `docs/generation.md` | geometric generators, FCC/BCC parity, neighbour-count analysis, the overload guard |
| `docs/capture.md` | PNG slice rasterisation, series capture, tiling, capture presets |
| `docs/persistence.md` | the `.casave` format and what it deliberately does not store |
| `docs/hud.md` | `FHudStats`, the Blueprint API surface, the generation-graph Slate widget |
| `docs/sonification.md` | the audio bridge, the log-space curve measurement, the MetaSound contract |
| `docs/testing.md` | what each automation test guards |
| `docs/tooling.md` | the MCP bridge, the hot-patch loop, Live Coding crash modes |
