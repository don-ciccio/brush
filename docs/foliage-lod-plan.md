# Foliage LOD & chunk-leverage plan — cull batches, not blades

**Status:** proposed (2026-07-14). Follow-up to the shipped `b_foliage`
(roadmap v1 #7) and the shipped terrain LOD rings (`chunk-lod-plan.md`, L1/L2
DONE). Deep-analysis + web-research pass on: *can we leverage the chunk-streamed
world to handle foliage LOD / billboards more cheaply than we do today?*

Short answer: **yes, but the lever is not the one it first looks like.** The
chunk grid is currently used only as a container the renderer iterates; every
LOD / billboard / cull decision is made **per-instance, on the CPU, every
frame** ([b_foliage.c] `FoliageSceneCb` + `BrushFoliageCull`). Industry practice
and our GL 4.1 ceiling agree: **cull whole batches (chunk / sub-tile), stop
touching individual blades every frame, and use the chunk hierarchy to extend
the *horizon* cheaply rather than to make the near field faster.**

## 1. The core finding

Per-frame, for every layer, `FoliageSceneCb`:

1. gets **all** resident chunks (`BrushWorldGetActiveChunks` — *not*
   frustum-culled),
2. walks each chunk's 4 m grid and runs a **per-instance** distance² + 60° cone
   test ([b_foliage.c] `InstanceVisible`),
3. appends survivors into per-layer CPU band buffers,
4. issues 2–3 `DrawMeshInstanced` calls — each of which **re-uploads** its whole
   instance VBO.

So the recurring cost is *(per-instance CPU cull across every near chunk)* +
*(per-frame re-upload of static matrices)*. That is the meadow thermal load, not
a leak (see the "meadow-grass thermal throttle" note).

The reference literature is blunt about the fix: **"use chunks and LODs
aggressively to cull out whole batches of grass… you should NOT try to cull
individual pieces of foliage"** (GPU Instancer). Unreal's HISM is the canonical
form — instances live in a spatial cluster tree, culling kills whole branches
before their transforms reach the GPU, turning *N* checks into ~*log N*. Our
chunk grid is a ready-made one-level version of that tree; we simply don't cull
on it.

## 2. What already exists (do NOT rebuild)

- **Terrain LOD rings** (`chunk-lod-plan.md`, DONE): `lodRadii[3]`
  full/half/quarter-res by Chebyshev radius, skirts hide seams, shadow casters
  distance-gated. **Foliage ignores this today.**
- **Scatter ring ⊂ terrain ring** (`foliage-plan.md` §4.2): foliage only
  scatters chunks within `scatterRadius = ceil(maxDrawDistance/chunkSize)+1`.
  With `drawDistance` ~80–120 m and 64 m chunks that is **~2–3 chunks** — which
  sits *entirely inside terrain ring 0 (full-res)*. **This reshapes the whole
  plan (see §4).**
- **3-tier per-instance LOD**: near mesh / triangle-budget-capped far mesh /
  cross-quad billboard impostor with baked albedo+normal atlas, lit live so it
  tracks day/night (`BrushFoliageBakeImpostor`). The *bake* machinery is good;
  it's the *per-instance banding + re-gather* that's the target.
- **Deferred-optimizations analysis** (`foliage-plan.md` §8): packed instances
  and persistent instance VBOs are already recorded as *measure-first, probably
  not worth it* — §6 here **reaffirms and does not override** that.

## 3. Hard constraints (bake in from line one)

- **macOS = OpenGL 4.1 → no compute shaders (4.3+), no SSBO.** Compute-driven
  GPU culling (the "GPU Instancer" path) is **off the table** on the primary
  target. On 4.1 the only GPU-culling routes are transform feedback or
  multi-draw-indirect — both heavy, and MDI still needs its buffers filled
  without compute. **Our wins are CPU-side.**
- **Fragment / overdraw-bound frame** (`perf-audit.md`, retina 4× note). CPU
  cull micro-opt improves headroom / thermals but will not move FPS on a
  GPU-bound frame. **The FPS lever is far-field fill**, i.e. fewer / cheaper
  distant fragments — which is exactly what a cheap far tier buys.
- **64 m chunks are coarse for whole-chunk LOD.** A whole chunk flipping LOD
  would seam every 64 m. The LOD granularity must be **sub-chunk** — the
  existing 4 m grid cell (or a 16 m coarsening) — plus dithered cross-fade.
- **Apple-Silicon unified memory makes the per-frame instance upload near-free**
  (recorded in `foliage-plan.md` §8). This is why persistent VBOs are *not* the
  headline win — see §6.
- **Draw-call count is a design metric** (`foliage-plan.md` §2): today the cull
  accumulates *all* chunks into one buffer per band → 2–3 draws total. Any
  per-chunk-buffer scheme trades that flat call count for more draws; it must
  justify the trade (§6).

## 4. The reframe: chunk extends the horizon; the *cell* is the LOD unit

Because the foliage scatter ring sits inside terrain ring 0 (§2), "give each
chunk a terrain-ring LOD tier" buys almost nothing *today* — every foliage chunk
is already full-res terrain, all within ~120 m. The 64 m chunk is too coarse to
be the LOD unit and too uniform (all near) to be the density unit.

So the leverage splits two ways:

- **Chunk granularity → cheap rejection + cheap horizon extension.** Use the
  chunk AABB (the view already carries `origin`/`size`/`maxY`) to reject whole
  chunks by frustum + distance *before* any per-instance work, and — the real
  prize — to drive **cheap far tiers that let `drawDistance` grow without
  overdraw**, pushing the foliage horizon *out of* ring 0 and into the coarse
  terrain rings where mirroring them finally pays (§5.5).
- **Cell granularity (4 m) → the actual LOD/banding unit.** Classify each grid
  *cell* by distance and move its contiguous instance block as a unit. This is
  the HISM leaf: fine enough to avoid a 64 m seam, coarse enough to kill the
  per-blade test.

## 5. Design, in priority order

### 5.1 Chunk-level cull before per-instance work (low risk, first)
In `FoliageSceneCb`, build each chunk's AABB from the view
(`origin`,`size`,`maxY`) and test it against the camera frustum + `drawDistance`
before descending. Skip the chunk wholesale on fail. Today the 60° cone (run
per blade) is the *only* frustum reject. Complements the planned horizon-
occlusion cull (`foliage-plan.md` §4.1) — that hides chunks behind hills; this
drops chunks outside the frustum. Cheap (hundreds of chunks), pure CPU.

**Pad the box or tall grass premature-culls.** The view's `maxY` is the terrain
top; foliage sits on it and rises above by up to `scale·meshHeight·(1+jitter)`,
so pad the AABB top by the layer's max foliage height. Pad XZ too, by a small
margin (clump radius + slope-lean — `FoliageAlignUp` tilts blades outward past
the base point), or grass at a chunk edge peeking into a frustum edge gets
clipped. maxY is the one that bites first (camera angled down a slope with tall
grass on the high side).

### 5.2 Band per-cell, not per-instance — but fade PER-INSTANCE (low risk)
The 4 m grid already stores each cell's instances contiguously in
`gridIndices`. Classify each **cell** by its centre distance and `memcpy` the
whole cell's transforms into the band buffer instead of testing every instance.
Cells fully inside one band become block copies; a cell straddling a band
boundary falls back to the current per-instance path. Adds a per-chunk fast path
("entire chunk within near band → all cells near, one copy"). Targets
`BrushFoliageCull`'s inner loop only — no data-model change. ~16× fewer distance
tests at typical density.

**The rule that makes this safe:** cell-banding quantizes only *which mesh/VBO*
an instance draws with (the discrete, expensive LOD switch); the **shrink/fade
must stay continuous per-instance in the vertex shader**, computed from the
instance's own world-pos vs the camera — never from the cell centre. Fade by
cell centre and all 16 blades in a 4×4 m cell shrink/pop on the *same frame* — a
blocky wavefront that reads as obviously artificial. The existing shader already
fades per-instance (`uFadeStart`/`uFadeEnd` off instance distance), so this is a
"don't regress it" rule, not new work: the cheap discrete swap is hidden because
the continuous per-instance fade already matches near and far meshes across the
boundary.

### 5.3 Cheap far tiers — the overdraw win (highest FPS leverage)
The cheapest far tier is **no grass geometry at all**: past the billboard band,
fade the grass tint into the ground and shorten it toward the terrain (via the
vertex shrink of §5.4), so the field reads as continuous colour rather than
cards. Standard open-world practice (UE via Runtime Virtual Texture; the manual
form is "fade grass to ground colour + push shorter/greyer"). We already have the
pieces: the terrain `lit.fs`, the splat/tint, and the "grass horizon seam"
colour-stipple work is the same idea from the terrain side. This is where the 4×
retina thermal budget is actually won, and it is what makes a longer horizon
affordable (§5.5). The existing billboard impostor band stays as the mid-far tier
between mesh and colour.

### 5.4 Vertex-shrink transitions — NOT dither (supersedes the dither idea)
An earlier draft (and `foliage-plan.md` Phase 3) proposed a dithered
(`interleavedGradientNoise` `discard`) LOD cross-fade. **Reject it.** Two reasons,
both decisive here:

- **No TAA to resolve it.** We run SMAA (spatial), so there is no temporal
  accumulator to average the alternating pixels — dither shimmers under motion.
- **`discard` breaks Hidden-Surface Removal on Apple-Silicon TBDR.** Alpha-test /
  `discard` disables early-Z and defeats the tile GPU's HSR, so a dither band
  *increases* overdraw — the exact opposite of the plan's goal. Note our **near
  grass is opaque blade geometry** (`BrushFoliageMakeGrassPatch` — real tris, no
  alpha cut), so it currently benefits from HSR; only the billboard atlas and
  imported alpha-card `.glb`s pay the discard tax. Adding dither would drag
  opaque grass down to their cost.

Instead, **shrink vertices toward the grounded base in the vertex shader** as an
instance nears a LOD/cull boundary. Because the mesh base is already baked to
Y=0 (AddLayer grounding), scaling Y (and XZ) toward 0 sinks the clump into the
terrain — achieving "fade grass to ground" (§5.3) with **no shimmer and no
fragment penalty** (shrinking tris just become sub-pixel and drop out). The
shrink factor is the same per-instance distance term the shader already computes
for `uFadeStart/uFadeEnd` (§5.2) — extend it from an alpha-fade to a scale.

**Limit — be honest about what shrink can't do:** it eases each tier's *far
edge* (near-mesh and far-mesh disappear smoothly, and the horizon fade), but it
does **not** cross-fade the discrete mesh→billboard *content* swap (two different
meshes). That swap stays a hard swap, hidden by the matched billboard silhouette
(the impostor is bbox-matched to the clump) + the shrink easing at the boundary.
Only if it reads badly, draw both tiers in a thin overlap band (double-draw cost)
— do not reach for dither.

### 5.5 Mirror terrain LOD rings — *conditional*, after §5.3
Only meaningful once §5.3 pushes the foliage horizon *past* terrain ring 0.
Then: bin chunks by live distance into foliage rings aligned to `lodRadii`;
outer rings **skip the near mesh entirely** (colour/billboard only) and **stride
the instance list** to thin density. Optionally expose the chunk's actual `lod`
level in `BrushWorldChunkView` so foliage and terrain agree exactly instead of
recomputing. Deferred until there's a horizon to tier.

## 6. Explicitly rejected / deferred (with the why, so it isn't re-litigated)

- **Compute-shader GPU culling** — impossible on GL 4.1 macOS. Do not architect
  around it. Transform-feedback / MDI culling are *technically* possible on 4.1
  but a large, fragile investment for a frame that is fragment-bound, not
  cull-bound. Rejected.
- **Persistent per-chunk instance VBOs** — reaffirming `foliage-plan.md` §8:
  (1) GL 4.1 has no `ARB_buffer_storage` persistent-coherent map, so the best is
  a manual orphaning ring bypassing raylib's instanced draw; (2) on Apple-
  Silicon unified memory the per-frame upload is already near-free, so the win
  is small; (3) it also **trades the flat 2–3-draws-per-layer design for
  per-chunk draws** (§3). Net: not the headline. If ever pursued, it is an
  engine-wide instanced-draw primitive (trees/debris/foliage), gated on a
  profile that actually names the upload. Deferred.
- **Per-tile HLOD impostor shell** (one baked card per sub-tile instead of many
  billboards) — only if §5.3's fade-to-terrain-colour proves visually
  insufficient. Higher effort (per-tile bake + streaming). Deferred behind a
  visual bar.
- **Packed 16 B instances** — deferred per `foliage-plan.md` §8; §4.2's scatter
  ring already bounds memory. Unchanged.

## 7. Phased rollout (each `make verify`-green, screenshot- + FPS-verified)

| Phase | Deliverable | Targets which bound | Proof |
|---|---|---|---|
| **F1** | Chunk-AABB frustum+distance reject (§5.1) + per-cell banding (§5.2) | CPU / thermal headroom | cull time drop vs baseline; identical frame visually; meadow soak stays cool |
| **F2** | Vertex-shrink LOD/cull transitions (§5.4) — extend the per-instance fade term to a scale-to-ground | quality (enables F3) | smooth shrink into terrain under motion; no shimmer; no added `discard` |
| **F3** | Fade far grass to terrain colour + shorten (§5.3); billboard band retuned as the mid-far tier | GPU / overdraw → **FPS** | vsynced FPS gain at a *longer* horizon; no hard grass→ground line |
| **F4** (cond.) | Foliage LOD rings mirroring `lodRadii`, incl. optional `lod` in chunk view (§5.5) | scale (only if F3 extends horizon past ring 0) | density/tier steps at ring boundaries hidden by F2/F3 |

## 8. Measurement discipline

- Benchmark each phase against a **fresh baseline run, vsynced FPS** for go/no-go
  (same rule as `chunk-lod-plan.md`); the meadow at 4× retina is the stress
  scene.
- State the bound each change targets (table above). F1/F2 are CPU/quality and
  will *not* show on a GPU-bound FPS counter — judge F1 on cull-time + thermal
  soak, not FPS. F3 is the FPS phase.
- The depth-prepass experiment (measured a *net loss*, correctly reverted) is the
  template: prototype the contained change, measure, keep only if the number
  moves.

## 9. Open decisions (resolve before coding)

- **Cell size — DECIDED: keep 4 m.** Since the fade is per-instance in the shader
  (§5.2), cell size is purely a CPU-batching detail — it only quantizes the
  mesh-swap boundary, which the per-instance fade already hides. 256 cells/chunk
  is free to loop, and 4 m keeps most cells inside a single band (fewer
  per-instance fallbacks). 16 m super-cells would make the bands too chunky for no
  CPU gain that matters. Not an open question anymore.
- **Fade-to-colour authority** — does the far grass colour come from the layer
  `tint`/`macro` or sample the terrain splat under it? Sampling the terrain
  blends seamlessly but couples foliage to the splat texture; layer-tint is
  self-contained. Lean self-contained first (matches "grass horizon seam").
- **Horizon target** — how far do we actually want grass to read? Sets whether
  F4 is ever needed. Pick a metres number against the meadow before F3.

## 10. Gotchas to respect

- Per-cell banding must keep placement **read-only** in the set (the worker-
  vs-main ownership split, `foliage-plan.md` §4) — band classification is
  main-thread scratch, never written back into the chunk's set.
- **Fade/shrink stays per-instance, never per-cell** (§5.2). Compute it from the
  instance world-pos in the vertex shader; drive it off the cell centre and whole
  4×4 m blocks pop on one frame (the blocky-wavefront artefact).
- **No dither, no added `discard`** (§5.4): SMAA can't resolve it and it breaks
  Apple-Silicon HSR/early-Z (raises overdraw). Use vertex-shrink-to-ground for
  the fades. Keep the near grass path opaque so it keeps HSR.
- **Pad the chunk AABB** before frustum-culling (§5.1): maxY by max foliage
  height, XZ by clump radius + slope-lean, or tall grass at frustum/chunk edges
  premature-culls.
- Fade-to-colour must not create a visible **grass→ground seam** at the swap —
  the vertex-shrink + colour match are what hide it (the failure the "grass
  horizon seam" note fixed from the terrain side).
- Thermal-throttle testing rule applies: the meadow can throttle and *not
  recover*; benchmark cold, watch for the sustained-FPS cliff, not just peak.
