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

// One bucket of the uniform spatial grid: a [offset, offset+count) slice of the
// set's flat `gridIndices` array.
typedef struct BrushFoliageGridCell {
  int offset;
  int count;
  int capacity;
} BrushFoliageGridCell;

// A set of static foliage instances + a spatial index over them. STATIC after
// build — the renderer only reads it. Per-frame visible data lives in the
// caller (see the cull functions), not here.
typedef struct BrushFoliageSet {
  // All instances (computed once at scatter).
  Matrix *transforms;
  Vector3 *positions; // instance base XZ(Y) — the cull's cheap distance test
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

// 2-tier cull: walk only the grid cells within `drawDistance` of viewPos,
// apply distance + horizontal-FOV tests, and APPEND survivors' transforms to
// the caller's near (dist < lodDistance) and far buffers, continuing from
// *nearCount / *farCount. Bounds-checked against maxNear/maxFar. Read-only in
// `set`. Accumulate across chunks by threading the same buffers + counts.
void BrushFoliageCull(const BrushFoliageSet *set, Vector3 viewPos,
                      Vector3 viewTarget, float drawDistance, float lodDistance,
                      Matrix *outNear, int maxNear, int *nearCount,
                      Matrix *outFar, int maxFar, int *farCount);

// 3-tier cull: near (< lodDistance), far (< billboardDistance), billboard
// (< drawDistance) — each appended to its own caller buffer, so the three
// bands become three draw calls with no cross-chunk ordering constraint.
void BrushFoliageCull3(const BrushFoliageSet *set, Vector3 viewPos,
                       Vector3 viewTarget, float drawDistance, float lodDistance,
                       float billboardDistance,
                       Matrix *outNear, int maxNear, int *nearCount,
                       Matrix *outFar, int maxFar, int *farCount,
                       Matrix *outBillboard, int maxBillboard, int *billboardCount);

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

// --- Zero-asset procedural defaults + the instanced shader (MAIN THREAD) -----
// A grass tuft: `blades` crossed vertical cards (world `height` x `width`),
// vertexColor.a = height along the blade (0 base, 1 tip) for the wind sway,
// soft radial normals, UV v mapping base->0 tip->1. Uploaded. This is the
// engine's zero-asset default mesh; a game swaps in a .glb per layer.
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
  float maxSlopeDeg;    // skip terrain steeper than this (0 = no limit)
  float windStrength;   // per-layer sway amount
  float farKeepRatio;   // far-LOD decimation (1 = no far LOD, share near mesh)
  Vector3 tint;         // albedo tint ((0,0,0) -> white)
  Vector3 macroLow, macroHigh; // low-frequency macro-color ramp endpoints
  Mesh mesh;            // uploaded source mesh ({0} -> the procedural tuft)
  Texture2D albedo;     // albedo/card (id 0 -> the procedural gradient)
} BrushFoliageLayerConfig;

typedef struct BrushFoliage BrushFoliage;

// Create the system: loads the shared shader + the zero-asset tuft/gradient.
BrushFoliage *BrushFoliageCreate(void);
void BrushFoliageDestroy(BrushFoliage *f);

// Add a layer (returns its index, or -1 if full). Copies the config; builds the
// layer's far-LOD mesh, material, and draw buffers.
int BrushFoliageAddLayer(BrushFoliage *f, const BrushFoliageLayerConfig *cfg);

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

#ifdef __cplusplus
}
#endif

#endif // B_FOLIAGE_H
