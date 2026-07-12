# Terrain Texture Painting (roadmap G) — Implementation Plan

The last leg of the content track (materials → models → **terrain paint**).
Height sculpting alone gives grey hills; this adds Terrain3D-style splat
painting — brush grass/rock/dirt/path onto the streamed terrain — reusing
the sculpt overlay machinery, the material library, and the BC texture
pipeline wholesale. Nothing here invents a new system; every piece rides
an existing one.

## What exists that this builds on

* **Sculpt overlay** (b_world): sparse height-delta tiles on the heightmap
  grid, allocate-on-touch, canonical tile ownership of grid range
  `[t*res, (t+1)*res-1]` (edge samples have ONE owner → seamless chunk
  borders), `sculptMutex` between main-thread painting and the worker
  bake, dirty-chunk rebake through the recycler (rebaking chunks keep
  drawing the old mesh), one blob format (`BSC1` .terrain) doubling as
  save file and undo snapshot.
* **Material library** (b_scene): named albedo/normal/displacement/AO
  sets with triplanar or UV projection, resolved through the ref-counted
  registry, BC-cooked, live re-import.
* **Editor sculpt mode**: ring cursor, radius/strength, op toolbar,
  whole-overlay undo snapshots (cmd+Z), `[ ]` radius keys.
* **Terrain rendering**: chunks draw as raw meshes through the lit shader
  (`BrushRenderSubmitMesh`, shared `w->material`) — full CSM/PCSS, point
  lights, post.

## Design

### 1. Weight overlay (engine, b_world)

A SECOND sparse tile overlay, exactly parallel to the height deltas:

```c
// per tile: (tileRes)^2 samples, RGBA8 = weights of layers 0..3
typedef struct SplatTile { int tx, tz; unsigned char *w; } SplatTile;
```

* Same keying, same allocate-on-touch, same canonical ownership (borders
  stay seamless for free), same mutex.
* Unpainted world = implicit `(255,0,0,0)` — layer 0 is the base coat.
  A world with no layers configured skips the whole path (zero-asset
  default stays the checker/vertex-color terrain, unchanged).
* Weights keep the invariant `sum == 255`: painting layer N raises N and
  renormalizes the rest (Terrain3D's model). 4 layers is the classic
  RGBA budget and enough for grass/rock/dirt/path.

API (mirrors sculpting):

```c
void BrushWorldPaint(BrushWorld *w, Vector3 center, float radius,
                     float strength, int layer); // smoothstep falloff, flow
```

Touched chunks mark dirty via the existing `SculptMarkDirty` path.

### 2. Per-chunk splat texture (worker bake + main-thread upload)

During the chunk's CPU bake (worker, under the mutex — same place the
height compose runs), fill `chunk->splatPixels` (hmRes², RGBA8) from the
weight tiles **including the one-sample apron** the heightmap already
uses — bilinear sampling across chunk borders needs the neighbour row,
and the apron pattern is already proven there. FinalizeChunk (main
thread, GL) uploads it as the chunk's splat texture; the recycler frees
it with the chunk. The worker never touches GL — splat is pixels on the
worker, texture at finalize, like everything else.

### 3. Shader: a splat branch in lit.fs

Extend lit.fs (NOT a bespoke terrain shader — one forward shader is the
engine's philosophy, and duplicating PCSS/point lights invites drift):

* `uSplatEnabled` + `uSplatTex` (per-chunk) + 4 layer albedo samplers +
  4 layer normal samplers, sampled with the existing local/world
  triplanar path at each layer's own `tile` scale.
* Weights from `uSplatTex` (bilinear, normalized in-shader as a guard),
  blend albedo and UDN-blend normals. Specular/AO params blend by the
  same weights.
* Texture unit budget check (GL 4.1 = 16): current lit draws bind
  albedo/normal/AO/height (4) + 3 shadow cascades = 7. Terrain-with-splat
  binds splat + 4 albedo + 4 normal + 3 cascades = 12. Fits; height/AO
  maps are per-material and stay off the terrain path in v1.
* Uniforms only bound for terrain draws — a `BrushMaterialProps`-style
  side channel on the mesh submit (`BrushRenderSubmitMeshSplat(...)` or a
  splat pointer in the cmd) keeps block/model draws untouched.

### 4. Layer set = material names (world.def + scene)

The four layers are entries from the SAME material library:

```
terrain_layer 0 grass
terrain_layer 1 rock
terrain_layer 2 dirt
terrain_layer 3 gravel
```

* b_scene parses/saves the lines; resolve/release rides
  `BrushSceneResolveMaterials` like everything else. The game hands the
  resolved layer textures to the world (`BrushWorldSetLayers(...)`).
* Layer materials should be triplanar-mode; `tile`, `spec`,
  `normalDepth` all apply per layer.

### 5. Persistence + undo: extend the .terrain blob

`BSC1` → `BSC2`: height tiles followed by weight tiles in one file.
Loader accepts BSC1 (heights only, weights empty) — existing .terrain
files keep working. The undo snapshot/restore blob gains the weight
tiles the same way: **one snapshot covers both overlays**, so a stroke
of paint and a stroke of sculpt share the same cmd+Z stack (restore
already zeroes tiles created after the snapshot; the rule extends to
weight tiles unchanged).

### 6. Editor: Paint tool inside sculpt mode

* Toolbar grows `Paint` next to Raise/Smooth/Flatten; when active, a
  layer picker (4 swatches showing each layer's albedo thumbnail — the
  registry already hands us the textures) and the existing
  radius/strength sliders (strength = flow).
* A "Terrain" section (Environment panel or the sculpt toolbar): four
  layer slots with material combos, writing the `terrain_layer` lines
  (g_dirty → saved with the scene).
* Brush stroke = `BrushWorldPaint` per frame while dragging, undo
  snapshot pushed at stroke start — all existing plumbing.
* Player: loads layers from the scene, sets them on the world, draws.
  The Play→hot-reload loop needs nothing new (.terrain saves on Save).

### 7. Stretch (explicitly NOT v1)

* **Auto-masks**: slope-keyed auto-rock / height-keyed auto-snow blended
  under the painted weights (Terrain3D's autoshader). Data-free — pure
  shader — so it can land later without format changes.
* **Surface-type query**: `BrushWorldSurfaceAt(w, x, z)` → dominant
  layer index, for footstep sounds/particles when audio lands.
* Per-layer height/AO maps (unit budget), more than 4 layers (second
  splat texture), splat-aware paths carving.

## Phasing

| Phase | Deliverable | Proof |
|---|---|---|
| **G1 engine** | weight tiles + BrushWorldPaint + apron-correct per-chunk splat texture + lit.fs splat branch + BrushWorldSetLayers + BSC2 blob/undo | BRUSH_TEST_PAINT harness paints a patch headless; screenshot shows blended layers across a chunk border |
| **G2 editor** | Paint tool + layer picker + Terrain layer slots (world.def lines) + snapshot-undo covering paint | paint in editor → Save → player shows identical terrain; cmd+Z reverts a stroke |
| **G3 polish** | slope auto-mask, layer normal maps if deferred from G1, surface query | side-by-side screenshot with/without auto-rock on a cliff |

## Gotchas to respect (from the sculpt/asset work)

* Tile border ownership is THE seam guarantee — weight tiles must use the
  identical `FloorDivL`/ownership math as height tiles.
* Worker discipline: weights composed on the worker under `sculptMutex`;
  GL upload only in FinalizeChunk.
* The splat texture needs the same apron treatment as the heightmap or
  bilinear filtering shows chunk-border lines.
* Renormalize on paint, and clamp in-shader anyway (BC compression of
  layer textures is fine; the splat texture itself stays RAW RGBA8 —
  it's runtime-generated, never cooked, never in the pak).
* Weight tiles ride the SAME dirty-rebake path as heights: a paint-only
  stroke still rebakes the chunk (mesh identical, splat regenerated) —
  acceptable; a splat-only fast path is an optimization for later.
* Editor harness first (BRUSH_TEST_PAINT), screenshots second — headless
  verification has bitten us every time we skipped it.
