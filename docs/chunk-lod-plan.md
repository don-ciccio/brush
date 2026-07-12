# Chunk LOD Rings — Implementation Plan

The cheap horizon win deferred from the clipmap discussion: today every
resident chunk bakes a FULL-resolution mesh, so view distance is capped by
triangle count (loadRadius 4 ≈ 9×9 chunks ≈ 290 m at 64 m chunks). LOD
rings keep full detail near the player and halve mesh resolution per
distance band — several-fold view distance at roughly the same triangle
budget, without touching the heightfield authority (sculpt, paint,
collision, ground queries stay exact where they matter).

This is deliberately geomipmapping-by-rings, not a clipmap: the chunk
system already unifies streaming/collision/paint, and the doc-recorded
escape hatch stands — if a project ever needs 10 km vistas, a clipmap can
replace the *renderer* later because the heightfield authority
(heightFn + overlays) is separate from mesh baking.

## Current facts the design must respect (b_world.c)

* Chunk = 64 m, `meshRes` 33 (2 m cells), `hmRes` 65 (1 m grid, +apron).
* Worker bakes heightmap + mesh + cooks the Jolt shape; main-thread
  finalize uploads. **Recycled chunks reuse GPU buffers via
  `UpdateMeshBuffer`, which assumes an UNCHANGED vertex count** — LOD
  breaks that assumption (see §4).
* `MAX_FINALIZE` per frame bounds upload hitches; unload ring has +1
  chunk hysteresis; `maxChunks = side² + 16`.
* Ground queries (`BrushWorldGroundHeight`) sample the CPU heightmap,
  not the mesh. Splat textures are `hmRes²` per chunk, independent of
  mesh density. Shadows: chunks submit as casters unconditionally.

## Design

### 1. Rings and configuration

```c
// BrushWorldConfig additions (0-terminated ring list, up to 3 entries):
//   lodRadii[0] = chunk radius of the FULL-res ring
//   lodRadii[1] = radius of the half-res ring, lodRadii[2] = quarter-res
// Unset (all 0) -> single full-res ring at loadRadius: today's behavior,
// zero change for existing games.
int lodRadii[3];
```

Per-chunk LOD is pure math — Chebyshev distance from the streaming
center vs the ring radii — so a chunk never needs to ask its neighbours
anything. `loadRadius`/`unloadRadius` derive from the outermost ring.

Mesh resolution per level: L0 = meshRes (33), L1 = 17, L2 = 9
(`(res-1)/2 + 1` — grid subsets, so LOD vertices lie exactly on the
full-res surface). Far rings also sample the **heightmap at the mesh
resolution** — heightFn calls dominate bake cost, and an L2 chunk needs
neither 1 m ground queries nor a collider (below).

Suggested defaults once proven: sandbox `{4, 8, 14}` ≈ 29×29 resident
≈ 930 m view. Triangle math: 81 full chunks ≈ 166 k + 208 half ≈ 106 k
+ 552 quarter ≈ 70 k ⇒ ~340 k tris vs 1.7 M if all 841 were full res.

### 2. Seams: skirts, not stitching (v1)

Adjacent rings meet at different resolutions → T-junction cracks. The
options, in industry order of fanciness: index-stitched transition strips
(geomipmapping proper), edge-vertex welding to the coarser grid, vertical
skirts. **v1 uses skirts**: every chunk extrudes its border row straight
down by a few metres (skirt depth scales with LOD cell size). Rationale:

* zero cross-chunk coupling — no neighbour LOD queries, no boundary
  rebakes when the player crosses a chunk line;
* correct against BOTH LOD seams and the existing floating-point hairline
  risk at borders;
* the cost is a ring of hidden triangles per chunk and rare grazing-angle
  artifacts — the trade every streaming hobby-to-mid engine makes first.

Edge-welding can replace skirts later behind the same bake API if the
skirts ever show (recorded as the L3 upgrade).

### 3. What LODs do NOT touch

* **Collision**: colliders only cook inside a `collisionRadius`
  (default = lodRadii[0] + 1). Distant trimesh cooking/memory disappears
  — nothing simulates 300 m away. Raycasts beyond the ring miss terrain:
  callers that care (editor cursor on far terrain) fall back to the
  heightmap query, which remains available for every resident chunk.
* **Ground queries**: near ring keeps the 1 m heightmap — foot IK,
  camera clamp, placement unchanged. Far rings answer from their coarser
  grid (fine for the camera far-clamp, the only far consumer).
* **Splat/paint**: weight tiles and per-chunk splat textures stay at
  their own resolution; painting far chunks re-bakes them at their
  current LOD like sculpting already does.
* **The rebase seam, job queue, atomic handoff**: untouched.

### 4. Finalize must handle vertex-count changes

The `UpdateMeshBuffer` recycle path is only valid when the new bake has
the same vertex count as the uploaded buffers. With LOD, a chunk crossing
a ring boundary rebakes at a different count. Finalize gains the second
path: count changed → `UnloadMesh` + fresh `UploadMesh` (and the CPU-side
arrays realloc in the bake). This also drops today's implicit invariant
instead of hiding it — the buffers-match fast path stays for same-LOD
rebakes (sculpt/paint strokes, the common case).

### 5. Ring-crossing churn control

Walking forward moves the center chunk; every ring boundary sweeps one
row of chunks into a different LOD → rebake. That's inherent to distance
LOD (geomipmapping pays it too); the mitigations:

* **Hysteresis**: a chunk only drops to a coarser LOD when it is
  `radius + 1` out, and only rises to finer when `radius` in — same
  pattern as the existing load/unload rings, kills boundary thrash.
* Rebakes ride the existing queue + `MAX_FINALIZE` budget; old meshes
  keep drawing until the new one lands (proven by sculpt rebakes), so
  the cost is temporary visual pop at distance, not hitching.
* LOD changes skip collider re-cooking outside `collisionRadius` (the
  expensive half of a bake vanishes for far chunks).

### 6. Shadows

930 m of resident terrain must not all feed the depth passes: shadow
submits gain a radial gate at the far cascade's reach (~220 m + margin).
Terrain beyond the last cascade cannot shadow anything visible.

## Phasing

| Phase | Deliverable | Proof |
|---|---|---|
| **L1** | lodRadii config + per-chunk lod + LOD-res heightmap/mesh bake + skirts + finalize realloc path + collisionRadius gate | BRUSH_HILLS at rings {4,8,14}: screenshot shows unbroken terrain to ~900 m, no cracks at ring boundaries; log reports tris/chunk counts; fps ≥ baseline |
| **L2** | LOD hysteresis + shadow radial gate + bake-budget tuning (finalize/queue) | walk 200 m straight (BRUSH_AUTO_MOVE): no fps dips beyond baseline noise; rebake counts logged per crossing |
| **L3** (later) | edge-welded transitions replacing skirts; far-ring splat simplification (dominant-color); geomorphing if pops read badly; horizon occlusion cull | side-by-sides |

## Gotchas to respect

* LOD meshes MUST sample the same composed surface (heightFn + sculpt
  delta) at grid-subset positions — never a re-filtered copy — or
  sculpt edits pop between rings.
* Skirt vertices get the border vertex's normal/color/UV (lighting must
  not crease at the top of the skirt).
* `maxChunks` grows to the outer ring (29² + slack) — CPU buffer memory
  is per-LOD-sized after §4, so the far rings stay cheap (~5 KB each,
  not ~100 KB).
* The editor uses smaller rings (its loadRadius is 3 today) — config,
  not code, differs.
* Thermal-throttle testing rule applies: benchmark against a fresh
  baseline run, vsynced FPS for go/no-go.
