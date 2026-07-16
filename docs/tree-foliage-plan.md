# Trees as Foliage Layers — C1 Spec (render tiers)

> Option C from the tree-system analysis: **foliage owns placement, the tree
> draw path owns rendering.** C1 = render tiers only. C2 (Jolt trunk
> colliders riding the chunk lifecycle) and C3 (editor polish, understory)
> are explicitly out of scope here.
>
> Donor reference: racer `src/trees.c` (492 lines) — three draw tiers,
> baked impostor billboards, shadow-LOD, analytic impostor lighting.

## 0. What we already have (audit result — better than expected)

`b_foliage.c` is already a **3-tier system**, not 2:

- near mesh → decimated far mesh (`farKeepRatio`) → **billboard impostor**
  (`billboardMesh`/`billboardMat`, `bbB` batch, `layer->billboardDistance`).
- The impostor bake (`BrushFoliageBakeImpostor`) renders **albedo AND
  encoded-normal atlases** through a manual ortho front view — the billboard
  is lit with the baked normals, so it tracks time-of-day. This is *better*
  than racer's flat-albedo impostor + `uLightMul` hack; we do NOT port
  racer's `uFlattenLighting` crossfade trick — our billboards are already
  lit consistently with the meshes.
- Chunk-streamed scatter (worker), biome gate, grow/avoid layers, painted
  density, per-layer quality scaling, crossfade windows with the
  offset-fade fix.

**What trees actually need on top — the real C1 scope:**

| Gap | Where it lives today |
|---|---|
| 1. Multi-submesh models (bark + cutout leaves) | conversion takes `mdl->meshes[0]` ONLY (editor/main.cpp:309, sandbox/main.c:435); layer stores one `Mesh` + one albedo per variant |
| 2. Shadow casting | foliage never renders into the CSM (grass only *receives*) |
| 3. Tree-scale distances | grass tuning: drawDistance ≤ ~200 slider, thin-keep decimates with distance (a vanishing TREE is a bug, a vanishing tuft is a feature) |
| 4. Canopy wind | wind rides vertex-colour alpha (0 base → 1 tip); tree GLBs don't author it |
| 5. Impostor atlas scale | bake sized for ~0.5 m clumps; a 15 m tree needs a taller atlas + correct cull radius |

## 1. Data model

### 1.1 `BrushFoliageLayerConfig` (b_foliage.h) — 3 new fields

```c
bool  tree;              // tree archetype: casts shadows, never thins with
                         // distance, taller impostor atlas, canopy wind ramp
float billboardDistance; // mesh -> impostor switch (m); 0 = auto
                         // (auto: grass = current derivation; tree = 90 m)
// castShadow deliberately NOT separate: tree => casts. Revisit if a
// non-tree layer ever needs shadows.
```

`tree` is an **explicit flag**, not height-based auto-detection — predictable
for authors, and a scene never changes behaviour because a model was re-scaled.

### 1.2 Per-variant submeshes (the meshes[0] wall)

```c
#define BRUSH_FOLIAGE_SUBMESHES 3   // bark + leaves (+1 spare)
```

Layer storage goes from `Mesh nearMesh[V]` to `Mesh nearMesh[V][S]` (+ same
for `farMesh`, `hasFar`, `material`), with `subCount[V]`. Config carries
`Mesh meshes[V][S]; Texture2D albedos[V][S]; int subCount[V];`.

- **Conversion** (both call sites): loop `mdl->meshCount` up to S; per
  submesh, resolve its OWN material's albedo via `meshMaterial[]`
  (the raylib glTF materials-shift-by-one gotcha is already handled by
  that pattern — keep using meshMaterial, never hard indices).
- **Draw**: the visible-instance batch (transforms) is per-variant and
  UNCHANGED; the instanced draw becomes an inner loop —
  `for s in subCount: DrawMeshInstanced(nearMesh[m][s], material[m][s], ...)`.
  Same batch, S draw calls. Grass (subCount 1) is bit-identical.
- **Far LOD**: decimate each submesh by `farKeepRatio` with the existing
  triangle-budget cap (the cap now applies to the SUM across submeshes).
- **Impostor bake**: render ALL submeshes of the variant into the same
  albedo/normal atlas passes (bake loop gains the submesh loop; framing
  from the union of submesh bounds).
- **Grounding**: the existing base→Y=0 bake must use the union-of-submeshes
  min Y (a leaf canopy usually starts above the trunk base; grounding on
  the LEAF submesh would sink the tree).

### 1.3 Scene format (b_scene) — v4 stays

`foliage` line gains fields 30–31: `tree` (0/1) and `billboardDistance`
(0 = auto). Loader: `fl->tree = (fn >= 30) ? ... : 0` — the established
optional-tail pattern; old scenes parse unchanged. Editor Foliage panel:
a "Tree" checkbox (flipping it ON nudges defaults: drawDistance 350,
lodDistance 50, density 0.004, farKeepRatio 0.25) + a "Billboard from"
slider shown for tree layers.

## 2. Render tiers (tree layers)

Distances (defaults; all per-layer, all × qualityScale):

```
0 ─ lodDistance(50) ─ billboardDistance(90) ─ drawDistance(350)
  near mesh        far (decimated) mesh    lit billboard      cull
```

- **Tier plumbing is the existing one** — `BrushFoliageCull` already splits
  near/far/bb batches with crossfade bands. Changes:
  - `BrushFoliageThinKeep` (distance thinning): **skipped for tree layers.**
    Every scattered tree draws until drawDistance, then the billboard just
    stops (at 350 m against the horizon fade this is acceptable; horizon
    fog is the cover).
  - Height-shrink crossfade: keep (it reads fine on trees — brief and
    masked by the offset fade windows).
  - Cull radius: `InstanceVisible`/cull-walk margins must use the layer's
    TALLEST variant bounds (a 15 m tree's cell can be off-screen while its
    canopy isn't). Audit the existing margin math against mesh height —
    likely just feeding the real bounds instead of a grass-sized constant.
- **Near tier stays INSTANCED with the foliage shader** (v1 decision).
  Racer drew near trees per-instance through the full lit shader (bark
  normal maps). We accept flat-lit bark at v1: one shader family, one
  code path, and trees read mostly as canopy at distance. Upgrade path
  (post-C1, only if close-up bark visibly hurts): route near-tier tree
  instances through `BrushRenderSubmitEx` per instance.
- **Canopy wind**: in the existing wind-bake step (the "wind-baked copy"
  of model meshes), if the layer is a tree and the mesh has no authored
  vertex alpha, bake `alpha = smoothstep(0.35, 1.0, y / meshHeight)` —
  trunk pinned, canopy sways; `windStrength` default low (0.15) for trees.
- **Impostor atlas**: bake height scales with mesh height —
  `rtH = clamp(64 * meshHeight, 128, 512)` per variant (grass keeps its
  current small atlas). Anisotropic filter + mips as racer does
  (BakeTreeImposter:117 — prevents distance shimmer).

## 3. Shadow casting

New render hook mirroring the scene callback:

```c
// b_render.h — invoked inside the CSM pass, once per cascade, with the
// light camera. Same contract as BrushRenderSetSceneCallback.
void BrushRenderSetShadowCallback(void (*cb)(void *user, Camera3D lightCam),
                                  void *user);
```

Foliage registers it in `BrushFoliageAttach`. The callback draws **tree
layers only**, **far-LOD submeshes only** (racer's rule: shadows always
use the LOD — trees.c:350), instanced, with a small depth-only foliage
shader variant (`foliage_depth`) that keeps the vertex path (wind) and the
leaf **alpha cutout** — solid shadow blobs from cutout canopies read wrong.

Culling, v1: reuse the camera-culled near+far batches from the last scene
draw (already resident) — sparse trees make over-inclusion cheap. If far
cascades show missing off-screen casters, add racer's sun-offset sphere
cull (trees.c:342) as the refinement.

## 4. Perf budget (honesty check)

At tree density 0.004/m² and 350 m draw distance: ~1,500 resident
instances, of which typically **< 80 in-frustum meshes** (near+far) and
~300 billboards. Cost drivers:
- near meshes: 80 × (LOD-capped) tris × 2 (shadow) — the knob is the
  far-LOD triangle budget, ALREADY capped absolutely (the 44→59 fps
  lesson: budget, not ratio).
- billboards: 2 tris each — noise.
- shadow pass: instanced LOD only, one extra draw per cascade per layer.

Gate: meadow + one tree layer at HIGH quality must hold the current frame
time within ~10% on the retina target; LOW preset must scale distances
exactly as grass does (it already multiplies drawDistance).

## 5. Out of scope (C1)

- **Colliders** (C2): trunk capsules at chunk finalize/unload.
- Octahedral impostors (single front-view bake first; billboards at 90 m+
  with anisotropic mips were fine in racer at similar ranges).
- Per-instance lit near tier (recorded upgrade path).
- Understory/bush auto-scatter around trees (racer bushes.c): a normal
  foliage layer with the same biomeId already approximates it.

## 6. Build order

1. Submesh storage + conversion loop + instanced submesh draws (grass
   regression gate: subCount==1 identical).
2. Union-bounds grounding + far-LOD across submeshes + impostor bake loop.
3. `tree` flag: thin-keep bypass, distance defaults, canopy wind bake,
   atlas sizing, cull-radius audit.
4. Scene fields 30–31 + editor Tree checkbox/defaults.
5. Shadow callback + foliage_depth shader + LOD instanced shadow draw.
6. Meadow test scene with a real tree GLB; visual + perf gates; commit
   per step where green.

## Progress (living log)

- Spec written 2026-07-16.
- **Steps 1+2 DONE (2026-07-16, committed together — they're inseparable in
  code):** per-variant submesh arrays `[V][S]` (S=3) on config + layer, sharing
  one instance-transform batch (one instanced draw per submesh); UNION-bounds
  grounding across submeshes; per-sub far-LOD with the triangle budget split
  by subCount (sum stays capped); impostor bake renders all submeshes into one
  atlas pair framed on the union (`BrushFoliageBuildBillboardMeshWH` extracted
  so card and atlas share bounds); both conversions loop `mdl->meshCount` with
  per-submesh material albedo. Gates: build clean; meadow editor shot
  pixel-identical to the pre-refactor baseline (subCount==1 path); sandbox 58
  FPS — and poppy flower HEADS now render (they were the lost second submesh
  all along; the old meshes[0]-only conversion was why poppies looked like
  "a single thin strand").
- **Fallout fixes (field-reported):** full-size clumps exposed that ALL
  placement tests were centre-point-only — wide models floated their downhill
  edge on steep/convex hills and hung canopies over roads ("avoid roads works
  for one layer not the other" = it worked for small meshes, failed for wide
  ones). Fix = footprint-aware placement: AddLayer computes each variant's
  union XZ half-extent (`cfg.baseRadius`, engine-filled); the scatter picks
  the variant/scale FIRST (same hash bits — deterministic-identical), then
  wide instances (r > 0.6 m) get (a) 4-probe MIN-grounding capped at 0.8r so
  the lowest footprint edge touches ground, and (b) 4 road-coverage probes at
  0.8r (centre threshold tightened 0.5 -> 0.35). Small grass keeps the
  single-tap fast path bit-for-bit.
- **Steps 3+4 DONE (2026-07-16):** `tree` flag end to end. Engine: tree
  distance defaults when unset (0.004/m², 350/50 m; billboard = authored
  `billboardDist` or min(90, draw/2)); thin-keep BYPASSED for tree layers
  (`thinning` param on BrushFoliageCull); cull margins from real bounds —
  per-layer `cullPad` = max variant extent (union radius OR height × scale ×
  jitter) widens the instance FOV-cone test (new `pad` param), the cell
  behind-camera test, and replaces the chunk-box `scale*10` guess; tree
  impostor atlases scale with mesh height (64/m, 256–512 px) and get
  mips + anisotropic (racer's anti-shimmer, trees.c:117). Scene: foliage
  fields 30–31 (`tree`, `billboardDist`), old lines parse unchanged. Editor:
  Tree checkbox (nudges 350/50/0.004/0.25/wind 0.15 once on toggle-ON) +
  "Billboard From" slider (0 = auto). **Canopy wind needed NO work**: the
  shader's bend is world-height-based with the base pinned (foliage.vs
  `h = worldPos.y - baseWorld.y`) — trunk still, canopy sways; only
  windStrength needs to stay low on tree layers (the nudge sets 0.15).
  Grass regression: meadow placement identical. NEXT: step 5 — shadow
  callback + foliage_depth cutout shader + instanced LOD shadow draw.
- **Field-reported tier bugs — FIXED (2026-07-16):** (1) trees invisible at
  distance + "mushroom" grow on approach = the billboard tier's height
  DISSOLVE spans the whole outer band (90→350 m) — a grass design ("melt
  into terrain") that erases trees by ~250 m and plays the grow backwards as
  you approach. Tree billboards now hold full height and dissolve only over
  the final 2·tFar before the cull edge. (2) "bark first, foliage later" =
  `BRUSH_FOLIAGE_FAR_MAX_TRIS 500` split per submesh gave the 15k-tri canopy
  ~250 tris (unreadable scraps) while the 250-tri trunk survived; tree layers
  now use `BRUSH_FOLIAGE_TREE_FAR_MAX_TRIS 6000` (~20 far-band trees keep it
  cheap). Chunk residency (14×64 ≈ 900 m) was NOT the limiter.
  OCTAHEDRAL impostors: still deliberately deferred — single front-view
  billboards live at 90 m+ where parallax error is small; revisit only if
  orbiting mid-range trees visibly flat-cards after these fixes.
- **LOD-ring scatter instability — FIXED (2026-07-16):** "foliage respawns at
  mid distance" + residual thin-air tree pops = the scatter's accept/reject
  (height band, slope) AND grounding sampled `heightAt` = the chunk's CURRENT
  LOD mesh, so a ring crossing (rebake at a new mesh res) returned a
  DIFFERENT instance set with different heights. Fix: new LOD-independent
  `heightFineAt` sampler (fine heightmap, sculpt/roads composed) drives ALL
  decisions; TREES also ground+probe on it (fully stable across rebakes —
  the sub-metre fine-vs-mesh error is invisible under a 10 m tree); grass
  keeps mesh grounding (sits on the rendered surface, moves WITH refines).
  Also: tree drawDistance now ignores the quality preset (LOW made trees
  vanish at 175 m — a bug, not a setting; racer never distance-culled trees).
  NOTE: the handle swap was already atomic (pending->live at finalize), so
  rebakes never blink — the instability was purely decision inputs.
- **THE actual bug — blank impostor atlases (2026-07-16, found by
  instrumentation after "nothing changed"):** the bake set `uFadeStart=0` but
  never `uFadeEnd` (default 0) → `fadeNorm = clamp(D / max(0, 0.001)) = 1` →
  `grassFade = 0` → **the model baked at ZERO HEIGHT: every impostor atlas in
  brush was BLANK.** Billboards were being drawn (debug: 7k tree billboards
  in-batch, hasImpostor=1) and rendering nothing. One bug = all field
  symptoms: trees invisible past bbDist ("don't exist at distance"),
  materializing at the mesh tier ("thin air on approach"), grass vanishing
  past its bbDist ~48 m ("respawning at mid distance"). Fix: the bake pushes
  uFadeEnd to 1e6 and disables the near fade-in. LESSONS: (a) the earlier
  "billboard verified +21%" claim was perf-only — the tier was never
  visually confirmed non-blank; (b) after two no-effect fixes, STOP and
  instrument (BRUSH_FOLIAGE_DEBUG=1 now logs per-tier counts + dumps the
  atlas PNG — both kept, env-gated). Grass far-field will look DENSER now
  everywhere (the billboard band finally renders); re-tune dissolve if the
  meadow horizon reads too thick.
