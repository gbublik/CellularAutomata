# Lattice geometry and cell shapes — `Automata/Grid/LatticeTransform.h`, `CellShapePresets.*`, `Automata/Simulation/LatticeNeighborhood.*`

How integer cell coordinates become world positions, and the five convex polyhedra that tile space by translation. Read before changing `GridToWorld`, adding a cell shape, or regenerating a cell mesh.

**Four of the five shapes work.** Fedorov's 1885 classification is *closed* — cube, hexagonal prism, rhombic dodecahedron, elongated dodecahedron, truncated octahedron, with 6/8/12/12/14 faces — so `CellShapePresets::GetAll()` has a final length rather than a growing one. Only the hexagonal prism is unimplemented; see the last section.

## `FLatticeTransform` — the geometry as one value

Before it, the lattice was a single `float CellSize` on `FCellGrid` plus the formula `Cell * CellSize` spread across the project. The forward mapping existed once (`FCellGrid::GridToWorld()`); the **inverse did not exist at all**, and in its place stood four hand-written divisions by `CellSize` — chunk rejection in `FDenseCellGrid`, the DDA ray-pick in `CellSelection`, cell-to-pixel projection in `CellRasterizer`, and the cull-box nudge in the orchestrator. On a cubic lattice all four are correct and the duplication is invisible; on a lattice with unequal per-axis spacing each breaks differently and silently.

So `CellSize` was **deleted from `FCellGrid` rather than supplemented** — that turned every one of those sites into a compile error instead of a potential fifth copy of the same formula. The type carries `CellWorldStep` (per-axis world step) and `Origin` (used by `FChunkGridView`, whose own `GridToWorld()` override disappeared as a result), and provides `GridToWorld`/`WorldToGrid`/`WorldToGridFractional`/`GridDeltaToWorld`/`WorldBoundsToCellRange`.

**It is a POD, deliberately against the project's usual convention.** Swappable behaviour here is normally an abstract class behind a `TUniquePtr` (`FCellularAutomatonComputeStrategy`, `FCellGridRenderer`, `FCellGrid` itself), but those are called once per step or per render, while this is called *millions of times per frame* (`BuildCellRenderData()`, `ComputeCellsBounds()`). Copying 56 bytes and inlining costs no virtual call — which is why moving to an anisotropic lattice did not slow rendering down but sped it up: where a `virtual GridToWorld()` used to be, there is now an inline multiply.

Two details worth not re-deriving: `WorldToGrid()` **rounds rather than floors**, because `GridToWorld()` returns the cell *centre* (cell `i` owns `[i-0.5, i+0.5)` of a step); and `GridDeltaToWorld()` exists separately so that code translating *by* some number of cells cannot forget to drop `Origin`, which must cancel in a difference.

**What it deliberately does not know: the shape.** Simple cubic, FCC and BCC have literally the same `GridToWorld()` — they differ in which nodes of `Z³` are populated and which count as neighbours. Cell extent, neighbour set, parity filter and mesh therefore live in the shape preset, not here.

## `FCellShapePreset` — shape as a consequence, not a setting

A shape is not an independent knob: it *follows* from four others — which nodes are populated (parity filter), which count as neighbours, how the lattice is stretched, and how much larger the mesh is than the step. A separate "shape" field would be a fifth source of truth arguing with the other four (pick "rhombic dodecahedron", leave a stale parity filter, and the picture matches neither). The preset sets all four at once and leaves; the fields stay ordinary and editable, so any combination — including a deliberately wrong one — remains legal, which `Generation.ParityFilter` relies on (Moore on FCC *must* break parity).

Same idiom as `FRulePreset`/`FRenderPreset`/`FStateGeneratorPreset`: a code-constant table, `BlueprintReadOnly` fields, one application function. It deliberately does **not** touch the rule or the generator — shape is geometry, the rule is dynamics, and being able to run one rule on different lattices is the whole point.

**Three of the five are one family.** Truncated octahedron, rhombic dodecahedron and elongated dodecahedron all live on the BCC sublattice (`ECellParityFilter::SameParity`) and differ *only* by the Z stretch:

| `LatticeZScale` | shape | faces |
|---|---|---|
| 1 | truncated octahedron | 14 (8 hexagons + 6 squares) |
| √2 | rhombic dodecahedron — BCC stretched by √2 **is** FCC | 12 rhombi |
| > √2 | elongated dodecahedron | 12 (8 rhombi + 4 hexagons) |

The threshold is exactly √2 because the face towards the neighbour at `(0,0,±2)` survives only while the midpoint to it (which moves out with the stretch) stays closer to the origin than to the nearest diagonal neighbour — and that distance is always √2. Past the threshold those two faces vanish and 14 becomes 12.

The rhombic dodecahedron is nevertheless written in the table as the FCC filter (`Even`) with stretch 1, not `SameParity` with stretch √2: both spellings give the same picture, but the first worked before stretching existed, has an integer lattice step, and does not depend on the precision of an irrational factor.

`ExpectedMeshAabb` is checked against the assigned `CellMesh` in `ApplyCellShapePreset()`, comparing **proportions normalised by X** (the renderer normalises by the mesh's X extent anyway). The engine validates mesh proportions nowhere at all, and a wrong mesh produces gaps or overlap — which looks exactly like a wrongly chosen lattice, so the mismatch is reported out loud, into the status line and not only the log. `ApplyCellShapePreset()` also compares the preset's `FaceCount` against `BuildNeighborOffsetsForAnalysis().Num()`: one face per neighbour is the *definition* of a Voronoi cell, and a mismatch means the pattern grows where cells do not visually touch.

## `ELatticeNeighborhood` — neighbour sets that shells cannot express

`ENeighborhood` is a closed model: four shells told apart by squared length in `Z³` (faces 1, edges 2, corners 3, far axes 4), and each of its 14 values equals the **union** of its shells. `Rules.NeighborhoodShells` asserts exactly that closedness. A value that is not such a union breaks every one of its assertions at once — so sets like this one live in their own enum instead.

`ElongatedDodecahedron12` is the 12 faces of the elongated dodecahedron: the 8 diagonals plus the **four in-plane far axes** `(±2,0,0)`, `(0,±2,0)`. Shells cannot say that — the far-axes shell contains all six, and on a Z-stretched lattice the face to `(0,0,±2)` no longer exists. Reaching for the ready-made `CornersFarAxes` (14) instead would count two neighbours the cell does not visually touch, and a pattern growing into thin air reads as a render bug within seconds.

The set is closed on the BCC sublattice: a diagonal flips all three parities at once, a far axis changes one coordinate by 2 and touches no parity. Its reach (`GetNeighborExtent()`) is 2 because of the far axes, exactly like `FarAxes`/`VonNeumann2`, so the GPU batch halo already handles it. Nothing below the rule — either compute strategy, the shader, the storage — knows about any of this: there, neighbour offsets are just a list of integer vectors.

**`AAutomataOrchestrator::BuildRule()` is the single place that decides which neighbour set is used**, and it prefers the lattice offsets whenever `NeighborhoodShape != Shells`. The rule is built in three places (`Next()`, `StepAsync()`, the Ctrl+Y histogram); duplicating the branch would let Ctrl+Y measure one neighbourhood while the simulation runs another, with no symptom beyond "the numbers look wrong for no reason". `BuildNeighborOffsetsForAnalysis()` makes the same choice for exactly that reason.

### Two traps

**A rule string cannot change the neighbour set.** `RuleStringParser` never touches `NeighborhoodShape` — grep confirms the field is written only by `ApplyCellShapePreset()` and by save/load — while `BuildRule()` gives it priority. So the `/M` or `/CFA` token in a rule string is silently ignored while a lattice shape is active, and only Birth/Survival/States apply. Getting back to the cube means setting `NeighborhoodShape = Shells` by hand.

**There is no button and no hotkey for the preset.** `ApplyCellShapePreset(int32)` is declared `CallInEditor`, but UE only renders parameterless functions as Details-panel buttons, so it is currently reachable only from Blueprint/HUD. Applying a shape by hand means setting the five fields the preset would have set, and then pressing **Y** — the parity filter takes effect at *generation* time, so without a regenerate the grid stays on the previous sublattice (**R** replays `InitialStateCells` and would restore the old lattice too).

## Tests

`Lattice.OrthogonalRoundTrip` (`WorldToGrid` is a true inverse of `GridToWorld`, including anisotropic steps) and `Lattice.ElongatedDodecahedronFaces` (the 12 offsets and their closure on the sublattice). Measured 2026-08-04: the full suite is **16 of 16 green**, including `Compute.CpuGpuParity` and `Compute.GpuBatchParity` on a real RHI — so the lattice refactor did not diverge the CPU and GPU paths.

Note `-nullrhi` silently *filters out* those two GPU tests rather than reporting them skipped, so a run with it reports 14 performed. Also: `-abslog=<path>` gets mangled when invoked through PowerShell — run the suite from bash, or the log is silently never written while the exit code stays 0.

## Cell meshes

Generated by script, not modelled by hand — `Tools/GenerateElongatedDodecahedron.py`, run through Blender in background mode. The body is built as the **convex hull of its vertices** rather than from a face list: a mistake in winding order is then impossible in principle, and the result is checked against the preset's numbers (vertices/edges/faces, face side counts, volume, AABB, and that the quads are genuine rhombi) *before* export, exiting non-zero on any mismatch.

The elongated dodecahedron is `{|x| ≤ 1, |y| ≤ 1, |z| ≤ (3 − |x| − |y|)/2}` in units of the planar step: 18 vertices, 28 edges, 12 faces, volume exactly 8, AABB 2×2×3.

Two things that cost time and are worth not repeating: **always verify by re-importing the exported FBX** (Blender and FBX disagree about axes, and a swapped one yields a plausible mesh lying on its side — 2×3×2 instead of 2×2×3), and **`LogStaticMesh: Error: Bad MeshDescription` on import is noise** — a stock Blender cube reproduces it, and meshes carrying it work fine.

### The border comes from the mesh, not the material

The cell material draws its outline from a distance-to-edge value arriving in `UV.x`, with `min(u, 1-u, v, 1-v)` computed in the graph ahead of it — which is why a cube gets a frame on all four sides of each face. Two consequences bind any mesh authored for it:

- **`UV.y` may not carry payload.** `min(v, 1-v)` is zero at `v = 0` and `v = 1`, which paints large dark wedges spreading from the corners of every triangle — on screen this reads as "the border is enormous". Keep `v` near 0.5 (±0.01; exactly constant would collapse the triangle to a segment in UV and leave the tangent basis undefined).
- **Distance to the *nearest* edge is a minimum over edges, so it is not linear and cannot be baked per vertex** — every vertex lies on an edge, so all of them would be zero and interpolation would give zero everywhere. Distance to *one* edge is linear, so each face is fanned from its centre (`bmesh.ops.poke`) and each triangle carries the distance to its own base: barycentric interpolation then reproduces the exact perpendicular distance, and near the edge — where the border is drawn — that edge really is the nearest one.

**The border comes out twice as wide as the cube's for the same `CellBorderWidth`, and that is unavoidable while the `min` is in the graph.** The `1-u` term hard-caps `u` at 0.5. A cube face is one lattice step across, so edge-to-centre is 0.5 and fits; the elongated dodecahedron's hexagonal face is a *whole* step from edge to centre. Baking the honest distance would make `1-u` win past `u > 0.96` and put a dark blob at the centre of every hexagon, so the range is compressed by half, halving the slope. Decided 2026-08-04 to keep it and halve `W` for such shapes; the alternative — feeding `d` straight in and dropping the `min` — would additionally require regenerating the cube with the same convention, and was declined.

## What remains: the hexagonal prism

The only one of the five still unimplemented, and it is not a mesh problem. All four working shapes live on the integer lattice and differ only in which nodes are populated and how the axes are scaled. The prism needs a **sheared** mapping, `X = (q + r/2)·a`, `Y = r·(√3/2)·a`, and `FLatticeTransform` has no shear term at all — only a per-axis step and an origin. So it is a change to the lattice type, not a new asset.

`ApplyCellShapePreset()` already refuses it explicitly (detected by the preset's expected mesh being wider in Y than in X) rather than applying settings that would draw hexagons on a cubic lattice. That refusal is deliberate: commit `8ef970a` reverted an earlier attempt that did the offsets first and the geometry later, which produced a picture that was plausible and wrong. The order therefore has to be the opposite — shear in `FLatticeTransform` first, with `Lattice.OrthogonalRoundTrip` extended to the sheared case, and only then the mesh.
