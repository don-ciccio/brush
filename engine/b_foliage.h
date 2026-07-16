/*******************************************************************************************
 *   b_foliage.h - CPU-side scatter / spatial-grid cull / LOD machinery for
 *                 instanced foliage (roadmap v1 #7 — see docs/foliage-plan.md)
 *
 *   A BrushFoliageSet is a bag of static instance transforms plus a uniform
 *   spatial grid over them. It is built ONCE (scatter + grid), on the world's
 *   worker thread (pure CPU, no GL), and is IMMUTABLE afterwards. Every frame
 *   the renderer culls it against the camera — distance + horizontal-FOV, on
 *   the grid so only nearby cells are touched — and appends the survivors into
 *   caller-owned per-band buffers.
 *
 *   Ownership split (the thread-safety design, docs/foliage-plan.md §4):
 *     - The SET holds only static, published-once data. The cull never writes
 *       it (it takes `const BrushFoliageSet *`), so the worker's output and the
 *       main thread's per-frame work never touch the same memory.
 *     - The VISIBLE buffers live in the caller (the per-layer renderer object),
 *       main-thread-only. The cull APPENDS into them (in/out counts), so many
 *       chunks accumulate into ONE pair/triple of buffers -> one draw call per
 *       LOD band, never per chunk.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_FOLIAGE_H
#define B_FOLIAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "b_world.h" // BrushWorld, BrushChunkCoord, BrushWorldChunkView

#include <raylib.h>
#include <stdbool.h>

// Up to this many model variants per foliage layer (a meadow mixes several
// meshes for variety — each instance randomly picks one).
#define BRUSH_FOLIAGE_MODELS_PER_LAYER 4
#define BRUSH_FOLIAGE_SUBMESHES 3 // per-variant submeshes (bark + leaves + spare)

// Hard triangle budget for the FAR LOD mesh. farKeepRatio is a *relative* ratio,
// which leaves a heavy imported model (thousands of tris) still heavy at
// distance — the dominant cost when thousands of far clumps draw. Capping the
// far mesh to a fixed budget makes dense high-poly model grass as cheap at range
// as the low-poly procedural tuft, with no visible loss (distant grass reads as
// texture). Light meshes already under the budget keep their authored ratio.
#define BRUSH_FOLIAGE_FAR_MAX_TRIS 500
// Tree layers get a far-LOD budget of their own: 500 (grass-tuned) split
// across submeshes shredded a 15k-tri canopy to ~250 unreadable scraps while
// the trunk survived — trees approached "bark first, foliage later". Sparse
// tree counts (~20 in the far band) keep 6000/instance well inside budget.
#define BRUSH_FOLIAGE_TREE_FAR_MAX_TRIS 6000

// One bucket of the uniform spatial grid: a [offset, offset+count) slice of the
// set's flat `gridIndices` array.
typedef struct BrushFoliageGridCell {
  int offset;
  int count;
  int capacity;
} BrushFoliageGridCell;

// Per-frame visible instances, routed by model variant so each variant draws
// with its own mesh in one instanced call. The cull appends into buf[modelIdx].
typedef struct BrushFoliageDrawBatch {
  Matrix *buf[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  int count[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  int cap;        // per-model buffer capacity
  int modelCount; // active variants (clamps stray indices)
} BrushFoliageDrawBatch;

// A set of static foliage instances + a spatial index over them. STATIC after
// build — the renderer only reads it. Per-frame visible data lives in the
// caller (see the cull functions), not here.
typedef struct BrushFoliageSet {
  // All instances (computed once at scatter).
  Matrix *transforms;
  Vector3 *positions; // instance base XZ(Y) — the cull's cheap distance test
  unsigned char *modelIdx; // which model variant each instance uses (0..N-1)
  int count;

  // Uniform spatial grid (built once, after scatter).
  float cellSize;
  float originX, originZ; // world XZ of the grid's min corner
  int gridResX, gridResZ;
  BrushFoliageGridCell *cells;
  int *gridIndices; // flat; each cell owns a contiguous [offset,+count) slice
} BrushFoliageSet;

// Invoked once per scatter-grid cell. Write 0..N instances into
// set->transforms/positions starting at *index, advancing *index per instance
// (must not exceed maxCount). Placement should be hashed in WORLD space so a
// cell is identical no matter which chunk scatters it (no seams across borders).
typedef void (*BrushFoliagePlaceFn)(void *userData, BrushFoliageSet *set,
                                    int *index, int maxCount, int gx, int gz,
                                    float baseX, float baseZ, float cellW,
                                    float cellD, float centerY);

// Scatter instances on a uniform grid over [center +/- width/2, depth/2].
// `density` is instances per square unit (pre-multiplier); `maxMultiplier`
// accounts for a placeFn emitting more than one instance per cell (clumps);
// `maxCountCap` hard-caps total instances. Allocates transforms/positions
// (trimmed to the actual count). No scratch/visible buffers are allocated —
// those belong to the caller now.
void BrushFoliageScatterGrid(BrushFoliageSet *set, Vector3 center, float width,
                             float depth, float density, float maxMultiplier,
                             int maxCountCap, BrushFoliagePlaceFn placeFn,
                             void *userData);

// Build the spatial grid index over the already-scattered instances. Call once,
// after BrushFoliageScatterGrid.
void BrushFoliageBuildGrid(BrushFoliageSet *set, Vector3 center, float width,
                           float depth, float cellSize);

// Cull routed by model variant: walk only the grid cells within `drawDistance`
// of viewPos, apply distance + horizontal-FOV tests, and APPEND each survivor's
// transform into the near (dist < lodDistance), far, or billboard band batch,
// indexed by the instance's modelIdx. Read-only in `set`. Accumulate across
// chunks by reusing the same batches (reset their counts once per frame first).
// Pass billboardDistance > 0 and a non-NULL bbB for the 3-tier split (near <
// lodDistance, far < billboardDistance, billboard < drawDistance); otherwise it
// is a 2-tier near/far cull (bbB may be NULL). `pad` widens the FOV-cone and
// behind-camera tests by the layer's instance extent (a 15 m canopy must not
// pop when its BASE leaves the cone); `thinning` false disables the outer-tier
// density taper (tree layers: every instance draws to the cull edge).
void BrushFoliageCull(const BrushFoliageSet *set, Vector3 viewPos,
                      Vector3 viewTarget, float drawDistance, float lodDistance,
                      float billboardDistance, float pad, bool thinning,
                      BrushFoliageDrawBatch *nearB, BrushFoliageDrawBatch *farB,
                      BrushFoliageDrawBatch *bbB);

// Free the set's instance + grid buffers (safe on a zeroed/partial set).
void BrushFoliageSetCleanup(BrushFoliageSet *set);

// --- LOD mesh builders (MAIN THREAD — they UploadMesh) ----------------------
// Deep-copy `source`, decimate the copy (BrushMeshDecimate, keepRatio of tris
// retained), and upload it as a standalone GPU mesh for the far LOD band.
Mesh BrushFoliageBuildLODMesh(Mesh source, float keepRatio);

// Same, but also cap the absolute triangle budget (keepRatio floored at 2%).
Mesh BrushFoliageBuildLODMeshTarget(Mesh source, float targetRatio, int maxTris);

// Build a 4-triangle cross-quad billboard sized to `source`'s bounding box,
// with soft radial "volumetric" normals and vertex-color alpha = height
// (0 base .. 1 tip). For the cheapest far band + a baked impostor texture.
Mesh BrushFoliageBuildBillboardMesh(Mesh source);

// Same card from an explicit half-width/height (the impostor bake frames the
// UNION of a variant's submeshes; card and atlas must use the same bounds).
Mesh BrushFoliageBuildBillboardMeshWH(float w, float h);

// --- Zero-asset procedural defaults + the instanced shader (MAIN THREAD) -----
// A realistic grass clump: `blades` thin, curved, tapered blades (5 verts /
// 3 tris each, ~1 cm wide, radial, bending outward with gravity droop) filling
// a `radius` circle up to `height` tall. vertexColor.a ramps 0(base)->1(tip)
// for the wind sway; UVs map to 3 vertical strips (for a striped blade card).
// Uploaded. THE zero-asset default — a proper grass look, not crossed cards.
Mesh BrushFoliageMakeGrassPatch(int blades, float radius, float height);

// A grass tuft: `blades` crossed vertical cards (world `height` x `width`),
// vertexColor.a = height along the blade (0 base, 1 tip) for the wind sway,
// soft radial normals, UV v mapping base->0 tip->1. The cheaper/blockier
// alternative to MakeGrassPatch (kept for billboards / low-detail).
Mesh BrushFoliageMakeTuft(int blades, float height, float width);

// A vertical gradient card (base color at v=0 -> tip color at v=1) with mipmaps,
// for the tuft to sample when a layer supplies no albedo texture.
Texture2D BrushFoliageMakeGradientTex(Color base, Color tip);

// Load engine/shaders/foliage.{vs,fs} with the mvp/view uniform and the
// `instanceTransform` attribute locations wired for DrawMeshInstanced.
Shader BrushFoliageLoadShader(void);

// --- Foliage system: layers streamed over the chunk world -------------------
// A layer is one plant type: a shared mesh + material scattered per chunk on
// the world's worker thread, culled and drawn across all active chunks in a
// couple of instanced calls. See docs/foliage-plan.md.
#define BRUSH_FOLIAGE_MAX_LAYERS 8

typedef struct BrushFoliageLayerConfig {
  float density;        // instances / m^2
  float drawDistance;   // hard cull (m) — the overdraw/thermal lever
  float lodDistance;    // near -> far LOD switch (m)
  float scale;          // base instance scale
  float scaleJitter;    // +/- fraction of random scale variation (0..1)
  float heightOffset;   // sink into ground to avoid floaters
  float minHeight;      // don't grow below this altitude
  float maxHeight;      // don't grow above this altitude
  float maxSlopeDeg;    // skip terrain steeper than this (0 = no limit)
  float windStrength;   // per-layer sway amount
  float farKeepRatio;   // far-LOD decimation (1 = no far LOD, share near mesh)
  Vector3 tint;         // albedo tint ((0,0,0) -> white)
  Vector3 macroLow, macroHigh; // low-frequency macro-color ramp endpoints
  // F3 grass-ground tint: greens the TERRAIN where this layer grows so the
  // ground reads as a grass field between/beyond the 3D blades (fills the bare
  // horizon; no ring). Drives the terrain shader (BrushRenderSetGrassGround)
  // via growLayer + drawDistance. groundStrength 0 -> off.
  Vector3 groundColor;   // grass-ground tint colour (sRGB)
  float   groundStrength;// 0..1 tint amount (0 = feature off)
  // Model palette: up to N mesh variants mixed per instance ({0} mesh ->
  // procedural tuft; id-0 albedo -> the model's own texture, else the gradient).
  // Each variant may carry several SUBMESHES (a tree GLB = bark + cutout
  // leaves, each with its own albedo); they share the variant's instance
  // transforms and draw as one instanced call per submesh. subCount[m] 0 -> 1.
  Mesh meshes[BRUSH_FOLIAGE_MODELS_PER_LAYER][BRUSH_FOLIAGE_SUBMESHES];
  Texture2D albedos[BRUSH_FOLIAGE_MODELS_PER_LAYER][BRUSH_FOLIAGE_SUBMESHES];
  int subCount[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  float meshScale[BRUSH_FOLIAGE_MODELS_PER_LAYER]; // per-variant scale x layer scale (0 -> 1)
  // ENGINE-COMPUTED (AddLayer fills from the union bounds; callers leave 0):
  // per-variant XZ half-extent in model units. Drives footprint-aware
  // placement — wide clumps ground to their lowest edge and keep their
  // canopy off roads (the centre-point tests alone let big models overhang).
  float baseRadius[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  int meshCount;        // 0 -> a single procedural tuft
  // Surface-layer auto-exclusion (reads the terrain splat weights).
  int   growLayer;      // grow only where terrain layer N dominates (-1 = any)
  int   avoidLayer;     // exclude where terrain layer N weight > threshold
                        // (-1 = none) — e.g. the road/paving layer
  float avoidThreshold; // 0..1 (0 -> default 0.5)
  bool  avoidRoad;      // exclude foliage from the road surface (road coverage
                        // mask, independent of the terrain layers)
  int   biomeId;        // grow only in this biome (-1 = all biomes); density is
                        // scaled by the biome weight so layers fade at borders
  // Tree archetype (docs/tree-foliage-plan.md): never thins with distance (a
  // vanishing tree is a bug, a vanishing tuft is a feature), tree-scale
  // distance defaults when unset (350/50/90), taller impostor atlas, and the
  // cull margins account for the real mesh extent. Canopy wind needs no flag:
  // the shader's bend is world-height-based with the base pinned — keep the
  // layer's windStrength LOW (~0.15) or a 15 m canopy sways metres.
  bool  tree;
  float billboardDist;  // mesh -> impostor switch (m); 0 = auto
} BrushFoliageLayerConfig;

typedef struct BrushFoliage BrushFoliage;

// Quality preset (the perf-audit's shipping knob, foliage slice). Scales the
// draw distance — foliage's real overdraw/thermal lever at 4x retina — and
// toggles shadow RECEPTION (the CSM taps in the grass shader). A future global
// BrushRenderSetQuality drives this; games can also set it directly.
typedef enum {
  BRUSH_FOLIAGE_LOW = 0,  // 0.5x distance, no shadow taps
  BRUSH_FOLIAGE_MED,      // 0.75x distance, shadows
  BRUSH_FOLIAGE_HIGH,     // full distance, shadows
} BrushFoliageQuality;

// Create the system: loads the shared shader + the zero-asset tuft/gradient.
// Initial quality is BRUSH_FOLIAGE_QUALITY (0/1/2, default HIGH).
BrushFoliage *BrushFoliageCreate(void);

void BrushFoliageSetQuality(BrushFoliage *f, BrushFoliageQuality q);
BrushFoliageQuality BrushFoliageGetQuality(const BrushFoliage *f);
void BrushFoliageDestroy(BrushFoliage *f);

// Add a layer (returns its index, or -1 if full). Copies the config; builds the
// layer's far-LOD mesh, material, and draw buffers.
int BrushFoliageAddLayer(BrushFoliage *f, const BrushFoliageLayerConfig *cfg);

// Drop every layer (frees their LOD meshes/materials/buffers), keeping the
// shared shader + procedural defaults. For the editor's live re-scatter: clear,
// re-add from the edited scene, then BrushWorldRebakeAll to re-scatter chunks.
void BrushFoliageClearLayers(BrushFoliage *f);

// Global wind shared by every layer (each scales it by its own windStrength).
void BrushFoliageSetWind(BrushFoliage *f, Vector2 dir, float strength);

// Bind to a world: installs the worker scatter/free hooks into the world config
// AND registers the render scene-draw callback. Call once, BEFORE the world's
// initial ring bakes (so the first chunks scatter). Returns the hooks via the
// config pointer the caller passes to BrushWorldCreate.
void BrushFoliageInstallHooks(BrushFoliage *f, BrushWorldConfig *cfg);
void BrushFoliageAttach(BrushFoliage *f, BrushWorld *world);

// Per-frame: advance wind/animation time and set the global draw-distance scale
// (<1 shrinks the whole foliage horizon toward the camera as it zooms out).
void BrushFoliageUpdate(BrushFoliage *f, float time, float distanceScale);

int BrushFoliageLayerCount(const BrushFoliage *f);

// Last frame's visible near/far instance counts for a layer (the editor's
// overdraw readout). Draw calls this frame = (near>0) + (far>0).
void BrushFoliageLayerStats(const BrushFoliage *f, int layer, int *nearCount,
                            int *farCount);

#ifdef __cplusplus
}
#endif

#endif // B_FOLIAGE_H
