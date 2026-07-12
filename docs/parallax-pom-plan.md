# Parallax Occlusion Mapping — Assessment & Plan

Upgrade the engine's dormant single-tap parallax to real **Parallax Occlusion
Mapping** (ray-marched depth + self-occlusion), starting with the cheapest,
most-contained path and gating the expensive terrain path behind distance +
quality presets. The paving road is the showcase.

## Where it stands (grounded in the code)

* **Single-tap parallax already exists and is DORMANT.** `lit.fs` offsets the
  UV once along the view ray in two paths:
  * triplanar / block path — `lit.fs:246-266` (samples `texture6` per axis, one
    offset, NO offset-limiting);
  * tangent-space / model-UV path — `lit.fs:268-279` (one offset, offset-limited
    by `/max(viewTS.z, 0.1)`).
* **Fully plumbed, never used.** Material `.def` has a displacement slot
  (`material name albedo normal displacement ao tile spec depth heightScale
  aoStrength uvproj`); `b_render` binds it to `MATERIAL_MAP_HEIGHT`/`texture6`
  and sets `uHasHeightMap` + `uHeightScale` (default 0.05, `b_render.c:327-342`).
  But **no height maps ship** and **every material's displacement is `-`**, so
  the path is dead code. The **splat/terrain path has no parallax at all**
  (`SampleLayer`, `lit.fs:179-205`).
* **Platform is fine.** Shaders are `#version 330` (GL 3.3): dynamic `for`
  loops, `dFdx/dFdy`, `textureGrad`/`textureLod` all available. POM is
  fragment-only — **no tessellation / geometry shaders**, so macOS's GL 4.1
  ceiling is a non-issue.
* **Perf context (from docs/perf-audit.md).** Post is ~58% of the frame and the
  laptop thermal-throttles under sustained 4× retina load. POM cost = steps ×
  covered pixels, so terrain (screen-filling) is the risk; props/close-ups are
  cheap.

## Locked decisions

1. **Fragment-only POM** (steep march + interpolate the crossing + offset
   limiting), `#version 330`. No tessellation.
2. **Reuse the existing plumbing** — `texture6`/`uHasHeightMap`/`uHeightScale`,
   per-material enable, the `.def` displacement slot. No new asset system.
3. **Always LOD it:** step count scales with view angle (more at grazing), and
   POM **fades to plain normal mapping with distance** — it never runs on far
   pixels. This is the main cost governor.
4. **Dominant-axis for triplanar/splat**, never 3× the march.
5. **Opt-in + preset-gated:** per-material (models/blocks) and per-layer
   (terrain) flags; a quality preset can force it off (Low) so it never touches
   the thermal budget on weak runs.
6. **Bootstrap without new art:** derive a temporary height map from albedo
   luminance so we can test the pipeline before sourcing/authoring real maps;
   swap real height maps in per material.

## Phase P0 — Shared POM core + a height map to test with

### P0.1 The core function (tangent space)
Add to `lit.fs` a reusable marcher used by every path:

```glsl
// Ray-march the heightfield in tangent space; returns the parallax-offset UV.
// viewTS = view dir in tangent space (points toward the eye). depthScale =
// uHeightScale. `steps` chosen by the caller (angle + distance).
vec2 ParallaxUV(sampler2D heightTex, vec2 uv, vec3 viewTS, float depthScale, int steps) {
    // Offset-limit: shrink the total sweep as the view grazes (kills swimming).
    vec2 P = (viewTS.xy / max(viewTS.z, 0.10)) * depthScale;
    float layer = 1.0 / float(steps);
    float curDepth = 0.0;
    vec2  dUV = P * layer;
    vec2  cur = uv;
    float h = 1.0 - texture(heightTex, cur).r;   // height stored 1 = top
    // Walk down until the ray passes under the surface.
    for (int i = 0; i < steps; i++) {
        if (curDepth >= h) break;
        cur   -= dUV;
        h      = 1.0 - texture(heightTex, cur).r;
        curDepth += layer;
    }
    // Interpolate the crossing between the last two samples (this is the "O").
    vec2  prev = cur + dUV;
    float after  = h - curDepth;
    float before = (1.0 - texture(heightTex, prev).r) - (curDepth - layer);
    float t = after / (after - before);
    return mix(cur, prev, t);
}
```

Notes: use `textureGrad(heightTex, cur, dFdx(uv), dFdy(uv))` for the marched
samples so the moving UV doesn't blow up the mip selection (banding). Clamp
`steps` and, optionally, `discard`/clamp when `cur` leaves `[0,1]` (POM has no
free silhouettes — a clamp reads as a flat border, a discard punches a hole; a
clamp is the safe default for tiling terrain).

### P0.2 A height map
The cook already carries a height channel. To unblock testing immediately,
derive a temporary height from albedo luminance (a `--height` mode in the cook,
or a one-off tool), then replace with real maps (authored, or CC0 sets) per
material. **Proof:** a material with a height map lights the same but now has a
`texture6` bound and `uHasHeightMap = 1`.

## Phase P1 — Tangent-space POM (models / UV surfaces) — cheapest first

Replace the single-tap offset at `lit.fs:268-279` with `ParallaxUV`, driving
`steps = int(mix(minSteps, maxSteps, 1.0 - viewTS.z))` (grazing = more) and a
distance fade:

```glsl
float pomFade = 1.0 - smoothstep(POM_NEAR, POM_FAR, distToCamera); // 0 far
if (uHasHeightMap > 0.5 && pomFade > 0.0) {
    vec2 pomUV = ParallaxUV(texture6, fragTexCoord, viewTS, uHeightScale, steps);
    uvMesh = mix(fragTexCoord, pomUV, pomFade);
}
```

Serves the rock props (`rock_*.glb`) and any UV-mapped material. Bounded screen
coverage → cheap. **Proof (headless):** a rock prop with a height map, viewed at
a grazing angle — F2 normals + lit view show recessed crevices and occluded
bumps that the flat normal-map version doesn't have; a flat-on view is nearly
identical (parallax only shows at angle).

## Phase P2 — Triplanar POM (textured blocks)

Apply the core to the block/material triplanar path (`lit.fs:246-266`), but
**march only the dominant axis** (the largest `triW` component) so it stays 1×,
not 3×. Offset-limit per axis. Same distance fade + angle step-count.

**Proof:** a textured block (brick/paving material) close up shows depth on the
face pointing most at the camera; cost measured with the perf ablation harness
stays within a set near-field budget.

## Phase P3 — Splat/terrain POM (the paving road) — gated

The road paints a terrain **layer**, and the splat path has no parallax, so the
paving-road showcase lives here. This is the expensive path — do it last and
gate hard:

* **One layer only.** The splat blends up to 4 layers/pixel; POM-ing all four is
  4× and absurd. March POM for the **dominant layer** (or a specific
  parallax-flagged layer, e.g. the paving), compute one offset UV, and reuse it.
* **Dominant-axis** triplanar (as P2), **near-only** (`POM_NEAR/FAR` a few
  metres), **mip-stepped** (fewer steps as the mip rises), and only on the
  full-res LOD-0 ring.
* Add a per-layer `parallax` flag to the terrain-layer config (a `BrushTerrain-
  Layer` field + `.def` token) and a matching height texture per layer.

**Proof:** the paving road up close shows 3-D cobbles / recessed grout; walking
away, POM fades to the flat texture with no pop; fps holds (measured), and Low
preset disables it entirely.

## Phase P4 — Self-shadow + presets (polish)

* **Soft self-shadowing:** a short second march from the hit point toward the
  sun; darken where the heightfield occludes it (big visual payoff on
  cobble/brick, ~1.5× the P-step cost — a separate toggle).
* **Quality presets:** `POM off / near-only / full` wired into the settings and
  the (planned) Low/Med/High tiers; `heightScale`, `POM_NEAR/FAR`, and max steps
  exposed as tunables (env + editor).

## Gotchas

* **Swimming at grazing angles** — offset-limiting (in the core) + angle-scaled
  steps handle most; keep `heightScale` modest (~0.03-0.08).
* **No true silhouettes** — POM can't round a mesh's outline; clamp UVs for
  tiling terrain, or accept flat borders. Real silhouettes need tessellation
  (out of scope).
* **Mip discontinuities** — the marched UV jumps, so sample with `textureGrad`
  using the *original* UV derivatives, or edges shimmer.
* **Triplanar = 3×** unless you restrict to the dominant axis. Always restrict.
* **Terrain seams** — height maps tile per material in the layer's own UV, so
  the parallax is within-tile and doesn't cross chunk borders; the height field
  is NOT the terrain heightmap (no collision/ground-query impact — POM is
  purely visual, geometry stays flat, so the character still walks the smooth
  surface: fine for cobbles, wrong for deep trenches).
* **Thermal budget** — measure every phase with the ablation harness at 4×;
  POM's whole job is to cost ~0 on far pixels. If near-field cost creeps,
  clamp steps and shrink `POM_FAR`.

## Sequencing

1. **P0** core + temp height map (bootstrap).
2. **P1** tangent-space POM on a rock prop — the low-risk proof.
3. **P2** triplanar POM on textured blocks.
4. **P3** splat POM for the paving road (gated) — the headline feature.
5. **P4** self-shadow + preset integration.

Each phase lands independently behind `uHasHeightMap` (and, for terrain, a
per-layer flag), verified with a grazing-angle before/after screenshot and a
perf delta. Nothing here touches gameplay — POM is visual only; collision,
ground queries, and foot-IK keep reading the smooth surface.
