# Foliage density painting — spec

**Status:** proposed (2026-07-13, `foliage` branch). The authoring tool that turns
foliage from "procedural grass everywhere" into a shippable, art-directed system —
the same loop UE (landscape grass + foliage painting) and Unity (terrain detail
density) use. It rides the existing sculpt/splat **tile machinery** (sparse
allocate-on-touch tiles, canonical ownership, region-dirty rebake), so it inherits
seamlessness, localized re-scatter, and `.terrain` persistence almost for free.

Companion to the **surface-layer exclusion** rule (separate, smaller task): that
gives *automatic* rules ("no grass on the road/rock layer, off cliffs"); this gives
*manual* art control (carve a trail, thicken a meadow, clear a clearing). They
compose — final placement = `base_density × surface_rule × painted_mask`.

## 1. What the artist does

Enter sculpt mode → pick the **Paint Foliage** tool → pick the active foliage layer
(the Foliage panel's selection) → brush density onto the terrain. Everywhere is the
layer's base density by default; **painting up thickens** (to `MAX_BOOST×`), **erase
(Shift) thins and carves out** (to nothing). Grass updates live under the cursor as the
painted chunks re-scatter.

(A future "Manual/paint-in" mode — nothing until painted — is deferred: it needs a
per-layer unpainted default, which an RGBA-packed tile shared by 4 layers can't hold
without overloading byte 0. Phase A is **Auto only** — neutral `M=1` everywhere,
which makes carve-out and thicken both clean.)

## 2. Data model — a third sparse overlay

The world already carries two sparse overlays on one grid (b_world.c): `tiles`
(sculpt height deltas) and `wtiles` (`SplatTile`, RGBA8 = 4 terrain-layer weights).
Add a third, identical in shape:

```c
// Sparse foliage-density tile: same grid + canonical ownership as sculpt/splat.
// RGBA8 = the painted density of up to 4 paintable foliage layers (0..255).
typedef struct FoliageMaskTile {
  int tx, tz;
  unsigned char *m; // tileRes*tileRes*4  (channel c = foliage layer c)
} FoliageMaskTile;
```

- **Sparse**: only painted regions allocate a tile (an unpainted world costs 0).
- **Canonical ownership** (the existing rule): tile (tx,tz) owns global grid indices
  `[t*tileRes, (t+1)*tileRes-1]` per axis, so chunk-edge samples belong to exactly
  one tile — no cross-border disagreement, placement stays seamless.
- **Value = density multiplier** `M = (byte/255)·MAX_BOOST` (MAX_BOOST ≈ 3). The
  neutral "1× base" is `byte ≈ 85`; absent tile and freshly-allocated cells default to
  85 (`M=1`), so unpainted = base everywhere. Paint toward 255 (3×), erase toward 0.
- **4 paintable layers** per tile (mirrors splat). Extends to 8 with a second tile set
  later; start with 4.

Sampler + writer mirror `SplatWeightsAt` / `SplatSampleRW` exactly:
```c
static float FoliageMaskAt(BrushWorld *w, long gx, long gz, int layer); // 0..1
static unsigned char *FoliageMaskRW(BrushWorld *w, long gx, long gz);   // paint
```

## 3. Paint API (b_world)

Mirror `BrushWorldPaint`/`BrushWorldPaintC`:

```c
// Add (or, erase=true, subtract) `strength` to foliage layer `layer`'s density
// mask under a `radius` brush at `center`, with smooth falloff. Marks the painted
// region dirty so ONLY those chunks re-bake + re-scatter (localized, fast — never
// BrushWorldRebakeAll). Grabbed under sculptMutex, like terrain paint.
void BrushWorldPaintFoliage(BrushWorld *w, Vector3 center, float radius,
                            float strength, int layer, bool erase);
```

The dirty-region rebake is the key efficiency: painting a 5 m brush re-scatters a
handful of chunks in a few ms, not the whole ring. This is why we ride the tile
machinery instead of a global mask texture.

## 4. Scatter integration — samplers + multi-emit boost

The `BrushFoliageChunkBake` hook currently takes one bound sampler (`heightAt`).
Generalize it to a **samplers struct** the world fills per chunk, so the scatter is
decoupled from every overlay's memory layout:

```c
typedef struct BrushChunkSamplers {
  float (*heightAt)(void *ctx, float wx, float wz);
  // Painted foliage density MULTIPLIER for a layer (0..MAX_BOOST); the world
  // composes the layer's Auto/Manual default. NULL-safe: absent -> 1.0.
  float (*densityAt)(void *ctx, float wx, float wz, int foliageLayer);
  // Terrain splat weights (0..1 per layer) — for surface-layer rules.
  void  (*splatAt)(void *ctx, float wx, float wz, float outWeights[4]);
  void *ctx;
} BrushChunkSamplers;
```

**Boost must not change the grid resolution.** Seamlessness depends on a *globally
fixed* world-cell grid; if a painted chunk scattered on a denser grid than its
unpainted neighbour, every shared border would tear. So the grid stays at the layer's
base density, and density is modulated **per cell by emitting 1..N jittered
instances** (raylib's `FoliagePlaceFn` already may emit multiple per cell). `MAX_BOOST`
(≈3) is the ceiling; `maxMultiplier`/`bufCap` size for it.

In `FoliagePlace`, compose the multiplier and emit:
```c
float m = densityAt(ctx, jx, jz, layer);          // painted, 0..MAX_BOOST
m *= SurfaceGate(splatAt, jx, jz, cfg);           // §4.1, 0 or 1 (or soft)
if (m <= 0.0f) return;
int n = (int)m;
if (Hash01(cell, 0) < (m - (float)n)) n++;        // fractional part
for (int k = 0; k < n; k++) EmitInstance(cell, k); // each jittered by Hash(cell,k)
```
`m ≤ 1` degrades to "emit 1 with probability m" (thinning); `m > 1` stacks extra
blades (thickening). All decisions come from the deterministic world-cell hash, so a
re-scattered chunk reproduces the identical layout and borders match — seamless.

### 4.1 Surface-layer auto-exclusion (folded into Phase A)
Per-foliage-layer rules read the terrain splat weights via `splatAt`:
- `growLayer` (−1 = any): grass only where terrain layer N is the dominant weight.
- `avoidLayer` (−1 = none) + `avoidThreshold`: **no grass where layer N's weight
  exceeds the threshold** — the "no grass on the paved road" case, since a road already
  paints its splat layer. Near-free and automatic (no painting needed).

`SurfaceGate` returns 0 when a rule fails, else 1 (a soft ramp near the threshold
avoids a hard grass edge along road shoulders). It multiplies into `m` above, so the
painted mask and the automatic rules compose: `final = base × surface × mask`.

These two fields join `BrushSceneFoliageLayer` (world.def, field-count back-compat)
and the Foliage panel (two combos), exactly like the other per-layer config.

## 5. Editor UI

A new sculpt-mode tool beside Raise / Smooth / Flatten / Paint / Road:

- **`Paint Foliage` tool** (`g_foliagePaintActive`, mutually exclusive with the
  others), painting the density mask of the **active foliage layer** (`g_selectedFoliage`
  from the Foliage panel — one selection drives both panel edits and painting).
- **Brush**: reuse the sculpt brush radius/strength + the ring cursor. Shift = erase.
  Left-drag paints; the click handler calls `BrushWorldPaintFoliage`.
- **Toolbar row**: when the tool is active, show the foliage-layer swatches (like the
  terrain Paint tool's layer swatches) so the artist picks which layer to paint, plus
  the per-layer **Auto/Manual** toggle.
- **Live**: the painted region's chunks re-bake automatically (dirty-region), so grass
  updates under the cursor — no explicit resync, no full rebake.

The Foliage panel (P7) already selects the active layer and shows stats; painting
extends that panel's workflow rather than adding a separate one.

## 6. Persistence + undo

`.terrain` (`BrushWorldSculptSave`/`Load`) already writes both overlays (sculpt
`tiles` + splat `wtiles`) under a version tag. Add `fmtiles` as a **third overlay**
+ a format version bump (old files load with no foliage mask; new engines read old
files fine). Snapshot/restore undo (`BrushWorldSculptSnapshot`/`Restore`) extends to
cover the mask tiles in the affected region — foliage paint joins the same Cmd+Z.

The layer *config* (density/distances/colours) stays in world.def (P6); the painted
*mask* is spatial binary data in `.terrain`, exactly the sculpt/splat split.

## 7. Phasing (each `make verify`-green, screenshot-verified)

- **A. Engine core (incl. surface-layer exclusion).** `FoliageMaskTile` overlay +
  `FoliageMaskAt`/`RW` + `BrushWorldPaintFoliage`; the `BrushChunkSamplers` refactor
  (height + density + splat); `FoliagePlace` multi-emit boost + `SurfaceGate`;
  `growLayer`/`avoidLayer` config (b_scene + b_foliage); localized dirty rebake. Verify
  headlessly with a scripted paint (thicken a strip, carve another; confirm the scatter
  thickens/thins) AND a road (confirm no grass on the paved layer) — no editor yet.
- **B. Editor tool.** The `Paint Foliage` tool, layer swatches, Auto/Manual toggle,
  ring cursor, live re-scatter. The visible authoring loop.
- **C. Persistence + undo.** `.terrain` v-bump with `fmtiles`; snapshot/restore.

Surface-layer exclusion (the automatic-rules companion) can land before or in
parallel with A — it shares the "sampler into `FoliagePlace`" seam.

## 8. Decisions (locked)

- **Paint to thicken, from day one (RESOLVED).** Mask multiplier `M ∈ [0, MAX_BOOST]`
  (MAX_BOOST ≈ 3). Byte `b` stores `M = (b/255)·MAX_BOOST`; the neutral "1× base" is
  `b ≈ 85`. Boost is done by **multi-emit** (§4), NOT a denser grid — a denser grid
  would break border seamlessness. Auto default (unpainted) `M=1`; Manual default `M=0`.
- **Surface-layer auto-exclusion in Phase A (RESOLVED).** `growLayer` + `avoidLayer`
  (+threshold) per foliage layer, read via `splatAt`; composes with the painted mask.
- **Brush falloff**: reuse the sculpt brush's falloff curve; revisit if grass edges
  read too soft.
- **4 vs 8 paintable layers**: one RGBA8 tile (4) now; second tile for 8 if needed.
- **Auto/Manual default**: per-layer flag persisted in world.def (a new `foliage`
  field, field-count back-compat). Default **Auto** (open-world-first).
- **Undo granularity**: per-stroke (snapshot on mouse-down, push on mouse-up), same as
  terrain sculpt.
</content>
