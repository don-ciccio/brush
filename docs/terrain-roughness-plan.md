# Terrain layer roughness — shiny mud vs matte grass

**Status:** proposed (2026-07-15). Supersedes an earlier draft that assumed a
from-scratch build. This version is grounded in what the engine ALREADY has: a
roughness/Oren-Nayar path in `lit.fs`, `roughnessDefault` on materials, a
`roughness` field on `BrushTerrainLayer`, an editor "Roughness Default" slider,
and on-load ORH compositing. The real work is small — and, critically, it must
drive the **specular**, not just the diffuse, or it won't look any different.

## 1. Why / the one thing that matters

Goal: different terrain splat layers read with different **shininess** — wet mud
catches a highlight, dry grass stays matte. Shininess is a **specular** cue.

The trap (why the naive version fails): `lit.fs` gates the terrain specular on a
BINARY flag —

```glsl
float isMatte = clamp(uSplatEnabled + uTriplanar, 0.0, 1.0); // 1.0 for ALL splat terrain
...
float specPower = mix(48.0, 4.0, isMatte);
float specStr   = uSpecStrength * mix(1.0, 0.05, isMatte);
```

— so every terrain pixel is forced matte. The `roughness` variable only feeds
the Oren-Nayar **diffuse** (`r2`). Setting per-layer `roughness` without touching
`isMatte`/`specStr` changes diffuse softness by a hair and leaves the whole
terrain matte. **The fix is to drive the terrain specular from the per-layer
roughness.**

## 2. What already exists (do NOT rebuild)

- **Material data** (`b_scene.h` `BrushSceneMaterial`): `char roughness[128]`
  (a roughness/ORH map path) AND `float roughnessDefault` (the scalar). Parsed
  and serialized already.
- **Engine layer field**: `BrushTerrainLayer` already has `float roughness`
  (`b_render.h`) — but `BrushSceneMaterialLayer` (`b_scene.c`) never sets it, so
  it's currently always 0.
- **Editor**: a "Roughness Default" slider and a Roughness map picker on the
  material (`editor/main.cpp`). Terrain layers ARE materials-by-name, so that
  slider IS the per-layer control — no new UI struct needed.
- **Shader hook**: `if (uSplatEnabled > 0.5) roughness = 0.95;` with a
  "haven't implemented per-layer roughness yet" comment — the exact insertion
  point.
- **ORH compositing**: `BrushAssetsSurfaceMap(ao, rough, height)` composites the
  packed map and is already called at resolve (`b_scene.c`
  `BrushSceneResolveMaterials`).

There is **no `BrushSceneTerrainLayer` struct** and no `BrushRenderSetTerrain` /
`BrushWorldSetTerrain` API — terrain layers resolve from materials
(`BrushSceneMaterialLayer` → `BrushTerrainLayer`) and stream via
`BrushWorldSetLayers`; per-layer uniforms bind in `ApplySplat`.

## 3. Hard constraint: the 16 texture-unit limit

`lit.fs` is at the hard 16 sampler-unit ceiling on this Mac (GL 4.1); exceeding
it makes the shader silently fall back to the default shader. Terrain therefore
CANNOT get per-layer surface/ORH **textures** (that's +4 samplers). Per-layer
roughness must be a **scalar uniform** (`uLayerRoughness`, one `vec4`, no
sampler). This is the ceiling and the reason the scalar approach is the right —
and only — one for terrain. (A material's roughness MAP still works for the
model/triplanar path, which samples the surface map; it just can't be per-layer
on the splat terrain.)

## 4. Design (the core: ~1 line each)

### 4.1 Populate the layer roughness — `b_scene.c`
In `BrushSceneMaterialLayer`, alongside the other `out->` assignments:
```c
out->roughness = m->roughnessDefault; // 0 -> shader default (0.95) handles it
```
The field already exists on `BrushTerrainLayer`; it's just never set today.

### 4.2 Bind the uniform — `b_render.c` `ApplySplat`
Next to where `uLayerTiles` / `uLayerSwizzled` are set, collect the 4 layers'
roughness (default matte where a layer is absent or unset) and push it:
```c
float rough[4] = {0.95f, 0.95f, 0.95f, 0.95f};
for (int i = 0; i < sp->layerCount && i < 4; i++)
  if (sp->layers[i].roughness > 0.0f) rough[i] = sp->layers[i].roughness;
SetShaderValue(g_r.lit, g_r.locLayerRoughness, rough, SHADER_UNIFORM_VEC4);
```
Add `int locLayerRoughness;` to the render state and
`GetShaderLocation(g_r.lit, "uLayerRoughness")` in `BrushRenderInit`.

### 4.3 Shader — the part that actually matters (`lit.fs`)
1. Declare `uniform vec4 uLayerRoughness;`.
2. **Weighted roughness must be computed where the splat weights `sw` are in
   scope** (they're local to the splat-albedo block — `sw` is OUT of scope down
   in the lighting section). Mirror the `gGrassMask` pattern: hoist a
   `float gTerrainRough = 0.95;` up top and set it inside the splat block, after
   the FINAL `sw` (post auto-slope / auto-height / height-blend):
   ```glsl
   gTerrainRough = clamp(dot(sw, uLayerRoughness), 0.0, 1.0);
   ```
3. At the existing TODO, replace the hard-coded matte with the per-layer value
   AND drive the specular off it:
   ```glsl
   if (uSplatEnabled > 0.5) {
       roughness = gTerrainRough;   // feeds Oren-Nayar diffuse (r2)
       isMatte   = gTerrainRough;   // <- THE fix: shininess follows roughness
   }                                //    (was implicitly 1.0 for all terrain)
   ```
   `specPower`/`specStr` already read `isMatte`, so a low-roughness layer (mud)
   now gets a tighter, stronger highlight and a high-roughness layer (grass)
   stays matte — no new specular code.

### 4.4 Editor (mostly done)
The material "Roughness Default" slider already authors this per layer. Optional
polish: surface that value (read-only or a shortcut) in the Environment →
Terrain Layers panel for discoverability, and re-`BrushWorldSetLayers` on edit so
it re-binds. No new data structure, no new parse/serialize.

### 4.5 Coherence with the grass sparkle
Terrain already carries two specular terms (`spec + grassSpec` in `lit.fs`): the
roughness/`isMatte` base spec and the far-field grass tip glint. Making grass
"matte" via roughness only affects the BASE spec; `grassSpec` (the distant
shimmer) is intentional and independent. Keep them separate, but sanity-check the
combined read so "matte grass" and "grass shimmer" don't fight up close (the
grass sparkle is distance-gated off near, so they shouldn't).

## 5. ORH bake-to-disk — SEPARATE, optional (Part 2)

On-load compositing (`BrushAssetsSurfaceMap`) is fine for iteration; baking to
disk is a shipping optimization, not part of the roughness feature. Do it only
if load-time compositing shows up. It is bigger than a button:

- **`BrushSceneMaterial` has no `surface` PATH field** — `surfaceTex` is DERIVED
  from `ao/roughness/displacement`. A bake needs: (a) add `char surface[128];`,
  (b) branch resolve — if `surface` is set, load it directly; else composite as
  today, (c) `BrushAssetsBakeSurfaceMap(ao, rough, height, outPath)` reusing the
  compositing logic + `ExportImage`, with a sane path
  (`assets/materials/<name>_orh.png`) and overwrite handling.
- **Decide re-editability**: clearing the source `ao/roughness/displacement`
  after bake makes it one-way (can't tweak channels later). Prefer KEEPING the
  sources (bake is a cache) and only preferring the baked file at load, so the
  material stays editable.

## 6. Sequencing (each `make verify`-green, screenshot-verified)
1. §4.1 + §4.2 + §4.3 — the whole "shiny mud / matte grass" feature. Small.
   Verify: set two terrain materials to roughness 0.2 (mud) and 0.95 (grass),
   paint both, confirm a moving specular highlight on the mud and none on grass
   under a low sun.
2. §4.4 editor discoverability (optional).
3. Part 2 (ORH bake) — only if load-time compositing is a measured cost; treat as
   its own task with the surface-path plumbing above.

## 7. Gotchas
- `sw` is local to the splat-albedo block — compute `gTerrainRough` there, not in
  the lighting section (the naive `dot(w, uLayerRoughness)` won't compile).
- Never add a per-layer surface SAMPLER to `lit.fs` — it's at 16 units; it'll
  silently fall back to the default shader. Scalar roughness only.
- `roughnessDefault` default: model props default to 0.4 (`b_render.c`); terrain
  defaults to matte 0.95 shader-side. Keep terrain's unset default matte so
  existing scenes don't suddenly go shiny.
- Order matters in `lit.fs`: set `isMatte` for terrain AFTER `gTerrainRough` is
  known but BEFORE `specPower`/`specStr` read it.
