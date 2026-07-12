# Terrain Tools — Smart Brush Constraints & Spline Roads (Revised Plan)

Supersedes the draft in `terrain_review_report.md`. Feature A (constraints) is
refined; Feature B (spline roads) resolves the destructive-vs-procedural
question the draft left open and specs `BrushWorldApplyRoad` in detail.

Grounding: the terrain overlay is a **destructive sparse-tile system**. Height
deltas live in `SculptTile.d` (`SculptSampleRW`), layer weights in
`SplatTile.w` (`SplatSampleRW`); both keyed by grid coord via `FloorDivL`,
guarded by `w->sculptMutex`, edited on the main thread and read by the worker
during the chunk bake. Crucially, **everything downstream reads the baked
overlay, not a live evaluation**: `BrushWorldGroundHeight` reads
`chunk->heightmap` (baked from `heightFn + SculptDeltaAt`, b_world.c:534/548),
colliders cook from that heightmap, and undo is a whole-overlay snapshot
(`BrushWorldSculptSnapshot`, editor `PushSculptUndo`). `Height(w,x,z)` is
heightFn ONLY (no delta) — the sculpt delta is what drives total→target.

## 0. Locked decisions

1. **Roads are a destructive stamp**, not a per-frame procedural overlay. An
   "Apply Road" carves the road into the SAME sculpt-delta + splat tiles a
   manual brush writes. Rationale: the mesh, Jolt collider, ground query,
   foot-IK, `.terrain` save, and cmd+Z **all already read the tile overlay**,
   so the road is correct everywhere for free and adds zero new threading.
   - **Trade-off (state it in the UI):** not live re-editable. Moving a node and
     re-applying *stacks* a second carve (tiles record no provenance). Re-edit
     workflow = cmd+Z the previous apply, adjust nodes, re-apply.
   - **Deferred v2:** true non-destructive editable roads = a road-eval pass
     wired into the worker bake (heightmap + splat) with a road-list lock. Much
     larger; not in scope here. The scene road data specced below is
     forward-compatible with it.
2. **Feature A ships first.** Low-risk, and the road tool reuses its slope/height
   sampling helpers.

---

## 1. Feature A — Smart Brush Constraints (masks)

Optional filters that make `BrushWorldSculpt` / `BrushWorldPaint` skip grid
samples failing a slope / height / layer test.

### 1.1 API

```c
typedef struct BrushConstraints {
  bool  checkSlope;                 // gate on surface steepness
  float minCosSlope, maxCosSlope;   // cos(angle): flat=1, vertical=0
  bool  checkHeight;                // gate on absolute Y
  float minHeight, maxHeight;
  int   targetLayer;                // -1 = any; else only where this layer dominant
} BrushConstraints;
```

Add a constrained variant rather than breaking the existing signatures:

```c
void BrushWorldSculptC(BrushWorld *w, BrushSculptOp op, Vector3 center,
                       float radius, float strength, float targetY,
                       const BrushConstraints *c);   // c == NULL -> unconstrained
void BrushWorldPaintC (BrushWorld *w, Vector3 center, float radius,
                       float strength, int layer, const BrushConstraints *c);
```
The old `BrushWorldSculpt/Paint` become one-line wrappers passing `NULL`.

### 1.2 Per-sample test (inside the existing grid loop, before writing)

```c
static bool PassConstraints(BrushWorld *w, long gx, long gz,
                            const BrushConstraints *c) {
  if (!c) return true;
  float step = w->gridStep, wx = gx*step, wz = gz*step;
  if (c->checkHeight) {
    float y = Height(w, wx, wz) + SculptDeltaAt(w, gx, gz); // TOTAL surface Y
    if (y < c->minHeight || y > c->maxHeight) return false;
  }
  if (c->checkSlope) {
    // Central-difference normal on the SAME field the bake/query use, at the
    // SAME 1-texel epsilon (else the filter disagrees with the visible slope).
    float e = step;
    float hL = Height(w,wx-e,wz)+SculptDeltaAt(w,gx-1,gz);
    float hR = Height(w,wx+e,wz)+SculptDeltaAt(w,gx+1,gz);
    float hD = Height(w,wx,wz-e)+SculptDeltaAt(w,gx,gz-1);
    float hU = Height(w,wx,wz+e)+SculptDeltaAt(w,gx,gz+1);
    // n = normalize(-dH/dx, 1, -dH/dz); cosSlope = n.y
    float nx = (hL-hR)/(2*e), nz = (hD-hU)/(2*e);
    float cosSlope = 1.0f / sqrtf(nx*nx + 1.0f + nz*nz);
    if (cosSlope < c->minCosSlope || cosSlope > c->maxCosSlope) return false;
  }
  if (c->targetLayer >= 0) {
    unsigned char wt[4]; SplatWeightsAt(w, gx, gz, wt);
    if (wt[c->targetLayer] < 128) return false;  // not the dominant layer here
  }
  return true;
}
```
All reads are already inside the `sculptMutex` the loop holds.

### 1.3 Notes carried from the review

- **This does not fully close "Gap #1" (destructive renormalization).** The
  layer mask only decides *whether* a sample is painted; the samples that ARE
  painted still fade every other layer proportionally (b_world.c:1177-1183). If
  you want "replace grass with gravel but keep rock," that is a *different*
  operation — a source-layer weight transfer — tracked separately below as
  **A′**. Ship the mask now; decide on A′ later.
- **UI in degrees, store cosines.** cos inverts the order ("max 30°" is a
  *minimum* cosine): the editor slider is degrees, convert on write.
- The slope epsilon MUST match the bake's (1 hmRes texel) or borderline slopes
  filter inconsistently with what's drawn.

### 1.4 (Optional) A′ — source-aware paint, the real Gap #1 fix

A `BrushWorldPaintReplace(w, center, radius, strength, dstLayer, srcLayer)`
that moves weight from `srcLayer` (or "all except a keep-set") into `dstLayer`
without touching the others — so gravel-over-grass leaves rock intact. Small,
but a distinct feature; not required for roads.

---

## 2. Feature B — Spline Roads (`BrushWorldApplyRoad`)

A road is a Catmull-Rom spline through control points. "Apply" **carves the
height** (flattens the corridor toward the spline's Y) and **paints the splat**
(drives the road layer to dominant in the corridor, fading at the shoulders),
straight into the overlay tiles.

### 2.1 Scene data (authoring record)

```c
// b_scene.h
#define BRUSH_SCENE_MAX_ROADS 32
typedef struct BrushSceneRoad {
  char  material[64];   // terrain-layer material name (resolved to a slot 0..3)
  float width;          // full-weight corridor width (m)
  float fade;           // shoulder falloff for both height + weight (m)
  Vector3 points[32];   // control points (Y matters: it's the road surface)
  int   pointCount;
} BrushSceneRoad;
// + BrushSceneRoad roads[BRUSH_SCENE_MAX_ROADS]; int roadCount; in BrushScene
```
Saved as text lines (matching `terrain_layer` style), e.g.
`road <material> <width> <fade> <n> x0 y0 z0 x1 y1 z1 ...`. This persists the
spline for re-editing; **the carve itself lives in `.terrain`** (already saved).
The scene road is authoring intent, not the source of truth for the surface.

### 2.2 `BrushWorldApplyRoad` — detailed spec

```c
// Carve + paint a road into the overlay (DESTRUCTIVE). Main-thread only.
// layerSlot = which of the 0..3 splat slots the road material occupies
// (resolved by the caller from road->material via the scene layer table).
// The caller snapshots undo BEFORE calling (PushSculptUndo) and the function
// marks the affected chunks dirty for async rebake.
void BrushWorldApplyRoad(BrushWorld *w, const BrushSceneRoad *road,
                         int layerSlot);
```

#### Step 1 — flatten the spline to a polyline (once)

Catmull-Rom needs phantom endpoints or the road won't reach its first/last
node. Duplicate the ends: `P[-1]=P[0]`, `P[n]=P[n-1]`. Sample each segment at a
fixed step (e.g. `max(2, ceil(segLen / w->gridStep))` sub-samples) into a
`Vector3 *poly` array (x,y,z world). Y is carried through so the road surface
follows node heights. Cache `poly` for the whole apply — do NOT re-evaluate the
curve per grid sample.

```c
Vector3 CatmullRom(Vector3 p0,p1,p2,p3, float t); // standard 0.5 basis
```

#### Step 2 — closest-point query against the polyline

```c
// Returns min XZ distance from (wx,wz) to the polyline, and the road surface Y
// interpolated at the projected point.
static float RoadNearest(const Vector3 *poly, int n, float wx, float wz,
                         float *outRoadY) {
  float best = 1e30f, bestY = 0.0f;
  for (int i = 0; i + 1 < n; i++) {
    Vector2 A = {poly[i].x,   poly[i].z};
    Vector2 B = {poly[i+1].x, poly[i+1].z};
    Vector2 AB = {B.x-A.x, B.y-A.y};
    float len2 = AB.x*AB.x + AB.y*AB.y;
    float u = len2 > 1e-6f ? ((wx-A.x)*AB.x + (wz-A.y)*AB.y)/len2 : 0.0f;
    u = u < 0 ? 0 : (u > 1 ? 1 : u);
    float cx = A.x + AB.x*u, cz = A.y + AB.y*u;
    float d = sqrtf((wx-cx)*(wx-cx) + (wz-cz)*(wz-cz));
    if (d < best) { best = d; bestY = poly[i].y + (poly[i+1].y-poly[i].y)*u; }
  }
  *outRoadY = bestY;
  return best;
}
```

#### Step 3 — AABB of affected grid, then the carve/paint loop

```c
float half = road->width * 0.5f, edge = half + road->fade;
// world AABB of all poly points, expanded by `edge`, snapped to grid.
float step = w->gridStep;
long g0x = floor((minX-edge)/step), g1x = ceil((maxX+edge)/step);
long g0z = floor((minZ-edge)/step), g1z = ceil((maxZ+edge)/step);

pthread_mutex_lock(&w->sculptMutex);
for (long gz = g0z; gz <= g1z; gz++)
for (long gx = g0x; gx <= g1x; gx++) {
    float wx = gx*step, wz = gz*step;
    float roadY, D = RoadNearest(poly, polyN, wx, wz, &roadY);
    if (D >= edge) continue;

    // corridor mask: 1 in the core, smoothstep to 0 across the fade band
    float m = 1.0f;
    if (D > half) { float t = (D-half)/road->fade; m = 1.0f - t*t*(3.0f-2.0f*t); }

    // (A) HEIGHT: drive total (base+delta) toward roadY, blended by m.
    //     Full override in the core (m=1 -> delta = roadY-base -> total=roadY),
    //     shoulders ease back to existing terrain. Identical shape to FLATTEN.
    float base = Height(w, wx, wz);
    float *d = SculptSampleRW(w, gx, gz);
    float targetDelta = roadY - base;
    *d += (targetDelta - *d) * m;                 // lerp(*d, targetDelta, m)

    // (B) SPLAT: lerp the weight vector toward (layerSlot = 255, rest = 0).
    //     A lerp of two sum-255 vectors stays sum-255, so NO additive
    //     renormalize -> this sidesteps Gap #1 entirely (rock isn't faded by a
    //     global renormalize; it's replaced by the road only inside the mask).
    if (w->layerCount > 0) {
      unsigned char *s = SplatSampleRW(w, gx, gz);
      float tgt[4] = {0,0,0,0}; tgt[layerSlot] = 255.0f;
      for (int i = 0; i < 4; i++) {
        float v = (float)s[i] + (tgt[i]-(float)s[i]) * m;
        s[i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
      }
      // rounding can drift the sum by ±1; if it matters, add the residual to
      // the dominant channel. Bilinear filtering hides ±1 in practice.
    }
}
pthread_mutex_unlock(&w->sculptMutex);

SculptMarkDirty(w, minX-edge, minZ-edge, maxX+edge, maxZ+edge);
```

That is the whole function. Notes:

- **No double-application.** One nearest-point per grid sample (min over
  segments) → one carve/paint. This is why the loop scans the road AABB rather
  than rasterizing per segment (per-segment quads overlap at joints and would
  blend a sample twice unless you add a scratch buffer — see Optimization).
- **Everything downstream is free.** Because we wrote the delta + weight tiles,
  the dirty rebake regenerates mesh, normals (recomputed from the delta,
  b_world.c:381), the collider, and `chunk->heightmap` — so the character walks
  the road, foot-IK plants on it, and `BrushWorldGroundHeight` returns the road
  surface. Zero extra plumbing.
- **`layerSlot` must be a configured slot.** The road material has to be one of
  the (≤4) terrain layers. The editor resolves `road->material` →
  `BrushSceneTerrainLayers` slot; if it isn't a terrain layer, the carve still
  runs but the paint is skipped (guard `layerSlot in [0,layerCount)`).
- **Thread safety** is identical to `BrushWorldSculpt/Paint`: main-thread writes
  under `sculptMutex`, worker reads under it. No new concurrency.
- **Undo** is the existing whole-overlay snapshot: editor calls
  `PushSculptUndo()` before `BrushWorldApplyRoad`, so one cmd+Z reverts the
  entire road as a single transaction.

#### Complexity & optimization

- Cost: `O(Ngrid × Nseg)` where `Ngrid` = road-AABB samples, `Nseg` = polyline
  segments. A long *diagonal* road has a large (mostly-empty) AABB — the
  cheapest mitigation is an early per-sample reject against each segment's
  expanded bounding box before the exact projection.
- If bake latency ever bites (it's a one-shot editor action, not per-frame),
  switch to **per-segment quad rasterization into a scratch grid** that stores
  `(minD, roadY)` per sample (min-D wins across segments), then a single
  carve/paint pass over the scratch. Bounds work to road *area*, not AABB, and
  still applies once per sample. Deferred until measured.

### 2.3 Editor UI

- New **Road** tool beside Sculpt/Paint (`BRUSH_EDITOR_TOOL=road` for headless
  shots, matching the existing tool env).
- **Add node**: left-click on terrain places a control point; snap its Y to
  `BrushWorldGroundHeight` on drop so it starts flush, then it's adjustable.
- **Manipulate**: per-node translate gizmo (reuse the block/model ImGuizmo
  path; nodes are just `Vector3`s in the selected `BrushSceneRoad`).
- **Width / Fade sliders** + a **live wireframe corridor preview** (draw the
  polyline offset by ±width/2 as line strips — pure viewport overlay, no bake).
- **Apply** button → `PushSculptUndo()` then `BrushWorldApplyRoad(...)`. Toast
  that Apply is a permanent stamp (cmd+Z to revert; re-apply stacks).

### 2.4 Persistence

- Scene saves the road splines (`road` lines) — authoring records.
- `.terrain` already saves the carved overlay (height + weights) — the surface
  of record.
- On load: the overlay carries the road; the spline records let you re-select
  and adjust nodes (with the cmd+Z-then-reapply caveat). This keeps the door
  open for the v2 non-destructive evaluator to consume the same `road` data.

### 2.5 Gotchas

- **LOD:** deltas live at hmRes (LOD-independent), so road *height* is correct
  on coarse rings; a narrow road's splat *edge* may look chunky where the mesh
  is decimated. Acceptable; note it.
- **Nodes with a bad Y** carve a trench/ridge — the snap-on-drop + gizmo is the
  guard.
- **Very sharp turns** can self-intersect the corridor; the min-D nearest-point
  handles it correctly (nearest wins), it just won't look great — a spline
  smoothing / min-radius warning is a nice-to-have.

---

## 3. Gap #3 (hardcoded shader `geoN.y < 0.85`) — optional, unrelated

Neither feature needs it. If the hard cutoff between single-projection and
triplanar bothers you, replace it with a `smoothstep(0.80, 0.90, geoN.y)` blend
between the two samples in `lit.fs` (a few lines, small extra cost on the
transition band only). Fold in or drop from the gap list.

## 4. Sequencing

1. **A — constraints** (`BrushWorldSculptC/PaintC` + `PassConstraints` + editor
   toolbar). Proof: painted/sculpted only within a slope+height band; headless
   `BrushWorldSurfaceAt` spot-checks.
2. **B — roads** (`BrushSceneRoad`, `BrushWorldApplyRoad`, editor Road tool).
   Proof: headless harness lays a 3-node road across a hill, applies, then
   `BrushWorldGroundHeight` along the centerline returns the flattened profile
   and `BrushWorldSurfaceAt` returns the road layer; screenshot shows a carved,
   gravel-painted corridor with terrain shoulders; character walks it.
3. **A′ / Gap #3** — only if wanted.

Each lands independently behind the existing dirty-rebake + snapshot-undo
machinery; none touches the worker or threading model.
