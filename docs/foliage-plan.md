# Foliage port plan — instanced scatter, culling, LOD, editor

**Status:** proposed (2026-07-13). Roadmap item **v1 #7** ("instanced foliage").
Ports the donor's proven scatter/cull/LOD machinery into a `b_foliage` engine
module, driven entirely by **world.def data** and authored in the editor. No
game code enters the engine; brush still ships **zero binary assets** (the
sandbox layer is a procedurally generated tuft).

## 1. Why this, why now

The streaming world (b_world) draws bare terrain. Foliage is the system that
turns "a heightfield scrolls past" into "a world." It is also the last big v1
open-world port — after it, the core is content-complete.

The donor already solved the hard parts and they transplant cleanly:

- **`src/foliage.c` / `foliage.h`** — the `FoliageSet`: static instance array +
  uniform spatial grid + per-frame distance/FOV cull that walks only nearby
  cells front-to-back, 2- and 3-tier LOD gathering, decimated-LOD and
  cross-quad-billboard mesh builders. **~95% engine-generic** (depends only on
  raylib `MemAlloc` + `MeshDecimate`). This is the spine of `b_foliage`.
- **`src/horizon_cull.c` / `.h`** — 1-D horizon occlusion map (64 azimuth
  slices, elevation angle per slice from analytic terrain samples); skips
  chunks hidden behind nearer hills for both CPU gather and GPU draw. Generic —
  its one game hook is `WorldHeight(...)`, which maps to our `heightFn`.
- **`src/mesh_decimate.c` / `.h`** — quadric-ish decimator the LOD builders
  need. Generic; port as-is to `engine/b_meshlod.c`.
- **`resources/shaders/grass.vs` / `grass.fs`** — instanced wind sway + distance
  fade + per-instance macro-color variation + soft billboard normals. Port the
  wind/fade/macro core; **drop** the player-push / trampling-trail uniforms
  (game interaction, roadmap v1 #8 territory) and rewire lighting to our sun +
  CSM shadow + volfog conventions.

What **stays in the donor** (do NOT port): `src/ground_cover.c` and
`crop_field.c`. These are content — crop-field regions, poppy/flower presets,
`GroundCoverMeadowLayers`, player trampling, the meadow color palette. Their
*reusable skeleton* (init a layer → scatter per chunk → gather visible across
chunks → two instanced draws) becomes the engine's generic layer path; their
*specific values* become world.def data the game/editor authors.

## 2. Hard constraints (learned already — bake these in from line one)

- **Thermal / overdraw discipline.** In the donor, grass overdraw at 4× retina
  caused *GPU thermal throttle that never recovered* ("meadow-grass thermal
  throttle" note). Draw distance is the master lever. Every layer ships with
  conservative `drawDistance` / `lodDistance` defaults, a global
  `distanceScale` the renderer pulls in as the camera zooms out (donor already
  does this), and the whole system honours the coming **quality preset**
  (Low = shorter distances, no billboard tier). Foliage is the first system
  wired to presets.
- **The worker thread never touches GL.** Scatter is pure CPU and runs during
  the chunk bake (it needs the freshly-composed heightmap anyway). Mesh/LOD
  upload and all draws are main-thread. Same atomic-handoff discipline as the
  terrain mesh.
- **Instanced draw-call count is THE metric.** Never draw per chunk. Gather
  every active chunk's visible instances for a layer into one near buffer + one
  far buffer and issue exactly **2 `DrawMeshInstanced` calls per layer per
  frame** (3 with the billboard tier). raylib rebuilds the instance VBO per
  call, so flat call count beats per-chunk batching every time.
- **Determinism / seamlessness.** Placement is hashed in **world space**, so an
  instance is identical no matter which chunk owns its cell — no seams, no
  repetition across borders. (Donor's `GroundCoverScatterChunk` already does
  this; keep it.)
- **Zero assets.** The engine generates a default grass tuft mesh + gradient
  texture procedurally (like it already generates the billboard cross-quad), so
  the sandbox has grass with no files. Real games point a layer at a `.glb`.

## 3. Module: `engine/b_foliage.{h,c}` + `engine/b_meshlod.{h,c}`

### 3.1 `b_meshlod` (port first, standalone)
`MeshDecimate(Mesh*, keepRatio)` verbatim from `src/mesh_decimate.c`, renamed
`BrushMeshDecimate`. No dependencies beyond raylib. Unit-testable in isolation;
`make verify` gate.

### 3.2 `b_foliage` core (port of `foliage.c`)
`BrushFoliageSet` = the donor `FoliageSet` (rename, drop nothing structural):
instances + spatial grid + visible buffers. Functions:
`BrushFoliageScatterGrid`, `BrushFoliageBuildGrid`, `BrushFoliageCull` /
`Cull3`, `BrushFoliageBuildLODMesh[Target]`, `BrushFoliageBuildBillboardMesh`,
`BrushFoliageSetCleanup`. Straight port; only the names and the `MemAlloc`/
`MeshDecimate` symbols change.

### 3.3 Foliage layers (generalized from `ground_cover.c`, data-driven)
A **layer** is one plant type. Runtime object owns the shared meshes/material
and the unified per-frame draw buffers; config is plain data (authored):

```c
typedef enum { BRUSH_FOLIAGE_FARLOD_3D, _BILLBOARD, _NONE } BrushFoliageFarLod;

typedef struct BrushFoliageLayerConfig {
  char  name[32];
  char  model[128];      // .glb tuft/clump; "" -> engine procedural tuft
  float density;         // instances / m^2
  float drawDistance;    // hard cull (m) — the thermal lever
  float lodDistance;     // near->far LOD switch (m)
  float billboardDistance; // far->billboard (m); 0 with farLod!=BILLBOARD
  float scale, scaleJitter;   // base scale + +/- randomization
  float heightOffset;    // sink into ground (avoid floaters)
  bool  clustered;       // low-freq noise gate -> natural patches vs even grid
  int   seedOffset;      // decorrelate layers sharing an area
  BrushFoliageFarLod farLod;
  Vector3 tint, macroTint;    // albedo + far macro-color target
  float windStrength;    // per-layer sway amount
  int   surfaceLayer;    // terrain splat layer this grows on (-1 = any)
  float maxSlopeDeg;     // don't scatter on cliffs (skip steep terrain)
} BrushFoliageLayerConfig;
```

`surfaceLayer` + `maxSlopeDeg` are the engine-generic replacement for the
donor's crop-field `GrassRegion` — instead of "inside the crop field," a layer
grows "where terrain splat layer N is painted" and "below slope X." That reuses
data the world already bakes (`BrushWorldSurfaceAt`, terrain normals), so
placement follows what the artist paints. Region/crop specifics stay in the
donor.

### 3.4 Shader `engine/shaders/foliage.{vs,fs}`
Port `grass.vs`/`grass.fs`, **keep**: per-instance distance fade
(`uFadeStart/End/NearEnd` matched to the layer's cull distances → zero pop),
distance size-shrink, Bhaskara wind sway, per-vertex macro-color FBM (drives
`uGrassTint`/`uMacroTint`). **Drop**: `uPlayerPos/Dir/Push`, `uTrailMap*`
(game interaction — leave commented hook points referencing roadmap v1 #8).
**Rewire lighting** to engine conventions: `uSunDir/uSunColor`, the CSM
`lightVP[]` + PCSS path from `lit.fs` (grass can use the cheap 1-tap variant),
ambient color, and sample the volfog so distant foliage sits in the same haze
as terrain. Alpha-tested, optional double-sided per layer.

## 4. Integration with the streaming world

Foliage rides the chunk lifecycle instead of duplicating it, and — critically —
**publishes through the chunk's existing `state` atomic, not a second
synchronization path.** The world already hands the worker's output to the main
thread race-free: `BuildCpu` fully writes `heightmap` / `mesh` (or
`pendingMesh`) / `pendingShape` / `splatPixels`, then does one
`atomic_store(&chunk->state, CHUNK_CPU_READY)` (seq-cst publish, the raylib #827
fix), and `FinalizeChunk` on the main thread swaps `pendingMesh → mesh` and
flips to `CHUNK_ACTIVE`. Foliage joins **that** handoff — one publish point, one
atomic, no foliage-specific lock.

The whole design turns on a clean split of ownership:

- **Per-chunk `BrushFoliageSet` = static data only** (`transforms`/`positions`/
  spatial grid), **written by the worker, immutable after publish.** The
  renderer never writes it — it only reads it during the per-frame cull.
- **Per-frame visible/scratch buffers live in the per-LAYER renderer object,
  main-thread-only** (moved OUT of the donor's per-set `visibleAll`/scratch).
  Because worker-written state and main-written state never overlap in memory,
  the race the naive port would create simply cannot happen.

Lifecycle:

- **Bake (worker thread).** After `BuildCpu` composes the heightmap
  (heightFn + sculpt + roads), it scatters each layer into a **pending** set
  (`pendingFoliage[layer]`, mirroring `pendingMesh`), sampling the just-composed
  heightmap for Y (no re-calling heightFn) and the terrain surface/slope for the
  `surfaceLayer`/`maxSlopeDeg` masks. Pure CPU — safe on the worker. Only chunks
  **inside the foliage scatter ring** (§4.2) scatter at all.
- **Finalize (main thread).** Inside `FinalizeChunk`, alongside the `pendingMesh`
  swap: swap `pendingFoliage[layer] → foliage[layer]` and free the old set. No
  per-chunk GPU work — instances are just matrices; LOD meshes / material /
  billboard RT are built **once per layer** at layer-create, not per chunk.
- **Edit / rebake — the race the report flags, and why it's already handled.** A
  sculpt/paint rebake re-scatters into `pendingFoliage` while the main thread
  keeps **drawing the old, still-live `foliage`**; the swap happens only at
  `FinalizeChunk` (main thread, between draws), never mid-frame. Identical to how
  `pendingMesh` already lets the old terrain mesh draw during a rebake. Foliage
  follows the ground it stands on for free, because it's on the same dirty path.
- **Draw (main thread, per frame).** `BrushFoliageDrawAll(world, camera,
  distanceScale)`: for each layer, walk active chunks (skipping horizon-occluded
  ones), `BrushFoliageCull` each chunk's **read-only** set into the layer's
  shared near/far buffers, then 2–3 `DrawMeshInstanced` calls total. Submits into
  `BRUSH_LAYER_OPAQUE` (and the shadow layer for near instances only, if the
  preset enables foliage shadows).

### 4.2 Scatter ring ⊂ terrain ring (memory + CPU bound)
Foliage only needs chunks within its **draw distance** (~1–2 chunks at
80–120 m), which is far smaller than the terrain `loadRadius` (default 4 =
9×9 resident chunks). Scattering the whole terrain ring would store — and
re-scatter on every sculpt — ~10× the instances we ever draw. So the scatter
ring is sized independently: `scatterRadius = ceil(maxLayerDrawDistance /
chunkSize) + 1`, clamped to `loadRadius`. This is the real memory/CPU lever
(see §8), and it bounds cost far more than any per-instance byte-packing.

### 4.1 Horizon cull (`b_meshlod`'s neighbor — put in `b_world` or `b_foliage`)
Port `horizon_cull.c`; replace `WorldHeight(seed,amp,x,z)` with a
`BrushWorldHeightSample(world,x,z)` accessor (thin wrapper over `heightFn`).
Cache each chunk's `maxY` during the mesh bake (one float, already have the
vertices). Build the map once per frame from the camera; test each chunk before
terrain submit AND foliage gather. This is also the world's other listed v1
TODO ("horizon occlusion cull") — landed together.

## 5. Data model — world.def + b_scene

Foliage layers are scene data, exactly like `terrain_layer` / `material` /
`road` lines. New line type (backward-compatible; old engines skip it):

```
foliage <name> <model|-> <density> <drawDist> <lodDist> <bbDist> \
        <scale> <jitter> <hOff> <clustered> <seed> <farLod> \
        <tintR tintG tintB> <macroR macroG macroB> <wind> <surfLayer> <maxSlope>
```

- `BrushScene` gains `BrushSceneFoliageLayer foliage[BRUSH_SCENE_MAX_FOLIAGE]`
  + `foliageCount` (mirror the road/material pattern).
- `BrushSceneLoad/Save` parse/emit the line (trailing-field back-compat via
  field count, same trick as the 14-field material line).
- A `BrushSceneFoliageLayers(scene, out)` resolver (mirrors
  `BrushSceneTerrainLayers`) hands the engine submit-ready configs;
  `BrushSceneResolveMaterials` resolves each layer's model via `b_assets`.
- Hot-reload: on mtime change, re-resolve + re-scatter (the whole resident ring
  re-bakes foliage — a few ms, fine for an editor action).

## 6. Editor integration (first-class — the emphasis)

Foliage is **procedural world config**, not per-instance placement, so it is
authored like terrain layers / road splines, **not** as a gizmo entity. It
lives in the **Environment tab**, in a new **`Foliage`** collapsing panel
directly under **Road Splines** (same idiom the user already knows).

### 6.1 Layer list
- A list of layers with **Add / Remove / Duplicate / reorder** (draw order =
  list order). Selecting a layer opens its inspector below (like selecting a
  road opens the spline inspector). No viewport gizmo; the "selection" is the
  active layer row.

### 6.2 Per-layer inspector (every config field → a widget)
- **Model**: text field + **drag a `.glb` from the Assets panel** (reuse the
  model-instance drag-drop), or "— (procedural tuft)". A small mesh/tri readout.
- **Density** slider (instances/m²) with a **live instance-count estimate** for
  the current draw radius.
- **Draw / LOD / Billboard distance** sliders — grouped, with the **thermal
  warning** surfaced: a colored hint when `drawDistance` pushes the layer past
  the preset's budget (this is the lever that cooked the donor).
- **Far LOD** combo: 3D mesh / Billboard / None (drives which meshes get built).
- **Scale + Jitter**, **Height offset**, **Wind strength**, **Tint /
  Macro-tint** color pickers, **Clustered** checkbox + **Seed**.
- **Surface layer** combo (populated from the scene's terrain layers: "any" +
  each painted layer) and **Max slope** slider — so a layer visibly snaps to
  where grass is painted / off cliffs.
- **Double-sided** checkbox.

### 6.3 Live authoring loop
Every widget edit sets `g_dirty` and a deferred `g_foliageResyncPending`
(mirror `g_roadResyncPending`): on the next idle frame, re-resolve the layer and
re-scatter the resident ring so the change is **visible immediately** without
stalling the drag. Distance/scale/tint changes that don't affect placement
(pure shader uniforms) skip the re-scatter and just update — cheap.

### 6.4 Stats & safety readout
A small stats block in the panel (and/or the existing debug HUD): per-layer
**total instances, visible near/far, draw calls, and est. frame cost**, plus
the world total. This makes the overdraw budget legible while authoring —
directly actionable given the thermal history.

### 6.5 Save / play
Wire into the existing Save (writes `foliage` lines via
`BrushSceneCaptureRenderSettings`-adjacent path) and the Play button (the game
loads the same world.def). Hot-reload already covers "edit file → world moves."

### 6.6 Phase 2 (flagged, not in first cut): density painting
The sculpt-brush infrastructure (ring cursor, radius/strength, sparse tile
overlay, rebake) can drive a **per-layer density mask** the same way splat
weights ride the sculpt tiles — "paint grass here, clear it there" on top of
the procedural rule. Reuses the paint pipeline wholesale; deferred so the first
cut ships the rule-based scatter.

## 7. Phased rollout (each phase `make verify`-green, screenshot-verified)

1. **`b_meshlod`** — port + verify decimation on a test mesh. (Standalone.)
2. **`b_foliage` core** — `BrushFoliageSet` scatter/grid/cull/LOD/billboard
   port. Unit-drive with a synthetic grid (no world yet).
3. **Foliage shader** — port + rewire to engine sun/shadow/ambient/volfog;
   procedural default tuft + gradient texture so the sandbox has grass with zero
   assets. **Billboard cross-fade (polish):** add a narrow-band dithered
   (stochastic `discard`) transition across the mesh→billboard switch to hide the
   silhouette pop — scoped to that band only (the near→far 3D switch keeps a
   similar silhouette; the existing distance height-shrink already dissolves the
   far cull edge). **Caveat:** we run SMAA (spatial), not TAA, so there is no
   temporal accumulator to resolve dither — keep the band narrow with a
   per-instance-stable (non-crawling) threshold and tune against shimmer under
   motion; lean on the height-shrink if dither reads as stipple.
4. **Per-chunk scatter + atomic handoff** — scatter into `pendingFoliage[layer]`
   on the worker, swap in `FinalizeChunk` beside `pendingMesh` (§4), per-layer
   shared visible buffers, cross-chunk unified draw, `distanceScale`, scatter
   ring sized from draw distance (§4.2). Sandbox: one grass layer over the hills.
   Verify seamlessness across chunk borders AND no tearing/flicker while sculpting
   (the rebake race).
5. **Horizon cull** — port, cache chunk `maxY`, wire into terrain + foliage
   gather. Measure the skip rate.
6. **world.def + b_scene** — `foliage` lines, resolver, hot-reload.
7. **Editor Foliage panel** — layer list + inspector + live re-scatter + stats
   + save (section 6).
8. **Quality-preset hook** — Low/Med/High scale the distances + billboard tier +
   foliage shadows (companion to the perf-audit preset work).

## 8. Open decisions (resolve before coding)

- **Layer cap**: `BRUSH_SCENE_MAX_FOLIAGE` — 8 is plenty for a meadow
  (grass/dry-grass/flowers/reeds); pick 8 unless we foresee biome stacks.
- **Foliage shadows**: near-band-only into the shadow layer, or skip entirely on
  Low? Donor grass mostly skipped self-shadow. Propose: off on Low, near-only on
  Med/High.
- **Wind authority**: per-layer strength now; a single global wind
  direction/gust (shared with any future cloth/tree sway) later — leave a
  `BrushSetWind(dir,strength)` seam.
- **Procedural default tuft**: reuse `BuildBillboardMesh`'s cross-quad, or a
  small fan of blades? Cross-quad is cheapest and already proven; start there.

### Deferred optimizations (measure first — NOT in the first cut)
These optimize costs we have not measured; the perf audit says the frame is
fragment/overdraw-bound, which none of them touch. Start with donor-proven plain
matrices + `DrawMeshInstanced`, and only reach for these if a profiler names
them. They are recorded here so the option (and its preconditions) isn't lost.

- **Packed instances (~16 B vs 76 B).** Store pos + yaw + scale packed, expand to
  a `Matrix` only for the *visible* subset at gather time — a ~5× memory cut that
  *preserves* the bake-once/cull-cheap split. Precondition: memory actually shows
  up (unlikely once §4.2 bounds the scatter ring). **Rejected alternative:**
  on-the-fly procedural placement ("0 MB") — it moves a per-candidate heightmap
  fetch + matrix compose into the per-frame cull (~hundreds of k fetches/frame on
  the main thread), trading non-critical memory for critical main-thread CPU on a
  GPU-bound machine. Only reconsider at 10 km+ world scale.
- **Persistent / ring-buffered instance VBO in `b_render`.** Avoids raylib's
  per-call instance-VBO churn. Preconditions/limits: (1) macOS caps us at **GL
  4.1 — true `ARB_buffer_storage` persistent-coherent mapping is unavailable**;
  the most we get is a manual orphaning ring (`glBufferData(NULL)` + `SubData`),
  requiring a custom instanced draw that bypasses raylib; (2) on Apple Silicon's
  unified memory the per-frame upload is near-free, so the win is small; (3) if
  ever done, it's an **engine-wide instanced-draw primitive** (trees/debris/
  foliage all share it), gated on a profile that shows the upload — not a
  foliage-first task.
</content>
</invoke>
