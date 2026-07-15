/*******************************************************************************************
 *   b_world.h - Chunk-streamed open world
 *
 *   Turns a bounded scene into a continuous world streamed in square chunks
 *   around a focus point (usually the player). The engine owns the hard part:
 *
 *   1. THREADING — each chunk's CPU build (heightmap, terrain mesh bake,
 *      collision geometry) runs on a background worker; the GPU upload +
 *      physics registration run ONLY on the main thread (raylib's GL context
 *      and — here — the render/physics state are single-threaded). Handoff is
 *      a lock-free atomic state flag (QUEUED -> GENERATING -> CPU_READY ->
 *      ACTIVE). The worker never issues a GL call.
 *
 *   2. SEAMLESS TERRAIN — the surface comes from ONE game-supplied function of
 *      world XZ (see heightFn). Neighbouring chunks feed the same world coords
 *      into it at shared edges and get identical heights, so terrain, normals,
 *      collision, and ground queries line up across borders with no skirts.
 *      heightFn MUST be deterministic and world-continuous; it runs on the
 *      worker thread, so it must be reentrant and touch no engine state.
 *
 *   3. REBASE SEAM — every draw passes through one WorldToRender chokepoint
 *      (identity today). At 1-4 km, float32 is precise enough; when a world
 *      outgrows that, shift the origin chunk in one place rather than hunting
 *      the codebase.
 *
 *   What a GAME supplies: the height function, chunk size / load radius /
 *   resolution, and an optional terrain texture. What stays racer-specific and
 *   is NOT here: road carving, building flatten boxes, foliage scatter (a
 *   separate system), horizon culling. The height function is where a game
 *   layers all of that on top of the base noise.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_WORLD_H
#define B_WORLD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "b_biome.h"   // BrushBiomeSample, BrushBiomeClimate
#include "b_physics.h"
#include "b_render.h" // BrushTerrainLayer

#include <raylib.h>
#include <stdbool.h>

// Integer chunk index. The world is addressed as a ChunkCoord + a local offset
// in [0, chunkSize). int32 covers a practically unbounded index range; the real
// limit is float precision of the local offset (handled by the rebase seam).
typedef struct BrushChunkCoord {
  int x, z;
} BrushChunkCoord;

// Bound samplers a per-chunk subsystem (foliage) uses during its worker bake to
// read this chunk's just-composed, chunk-local surface + overlays — all
// lock-free (the arrays are immutable for the bake's duration). World XZ in.
typedef struct BrushChunkSamplers {
  float (*heightAt)(void *ctx, float wx, float wz);       // terrain Y
  float (*densityAt)(void *ctx, float wx, float wz, int foliageLayer); // paint multiplier (0..MAX_BOOST; 1 = base)
  void  (*splatAt)(void *ctx, float wx, float wz, float outWeights[4]); // terrain layer weights 0..1
  float (*roadAt)(void *ctx, float wx, float wz);         // road surface coverage 0..1 (for foliage exclusion)
  void  (*biomeAt)(void *ctx, float wx, float wz, BrushBiomeSample *out); // dominant 2 biomes + blend
  void *ctx;
} BrushChunkSamplers;

typedef struct BrushWorldConfig {
  unsigned int seed;

  // Terrain surface: return world-space Y at (wx, wz). Called on the WORKER
  // thread — must be reentrant, deterministic, and world-continuous (two
  // chunks sharing an edge must agree at the shared coords). `user` is passed
  // through from heightUser. NULL -> flat world at y = 0.
  float (*heightFn)(void *user, float wx, float wz);
  void *heightUser;

  float chunkSize;  // metres per chunk side (0 -> 64)
  int loadRadius;   // chunks kept resident around the focus (0 -> 4)
  int meshRes;      // terrain vertices per chunk side (0 -> 33; 2 m cells)
  int hmRes;        // heightmap samples per chunk side (0 -> 65; 1 m cells)

  // LOD rings (Chebyshev chunk radii): [0] = full-res ring, [1] = half-res,
  // [2] = quarter-res. Distant chunks bake at ((meshRes-1) >> lod) + 1
  // vertices/side, cutting triangle count so view distance isn't capped by
  // it. Border skirts hide the inter-ring seams. Leave all 0 for a single
  // full-res ring at loadRadius (unchanged legacy behaviour). When set, the
  // OUTER radius overrides loadRadius; heightFn resolution, ground queries,
  // splat, and colliders (near ring only) are unaffected.
  int lodRadii[3];

  // Per-chunk terrain collider registered with this physics world (NULL -> no
  // terrain collision). Colliders stream in/out with their chunks.
  BrushPhysics *physics;

  // Optional tiling ground texture (0 id -> flat vertex-colour terrain, shaded
  // green lowland / grey rock by slope). Tiles in WORLD space so it doesn't
  // reset per chunk.
  Texture2D groundTex;
  float texMetresPerTile; // world metres per texture repeat (0 -> 4)

  // Initial splat layers, applied BEFORE the blocking initial-ring bake so the
  // first chunks are textured on frame 1 (no ~1s splat pop-in from a post-
  // create BrushWorldSetLayers marking every chunk dirty). Equivalent to that
  // call minus the re-bake; a later matching SetLayers is a no-op. layerCount
  // 0 -> plain groundTex / vertex-colour terrain (auto-slope + layer heights
  // are shader-side and can still be set after create without a pop).
  BrushTerrainLayer layers[BRUSH_TERRAIN_LAYERS];
  int layerCount;

  // Per-chunk subsystem hook (instanced foliage). `chunkBake` runs on the
  // WORKER during the chunk build, once the final heightmap is composed, and
  // returns an OPAQUE handle for this chunk (pure CPU — no GL). The world
  // stores it as pending and publishes it atomically with the mesh, swapping it
  // live in the main-thread finalize; the previous handle is released via
  // `chunkFree`. `chunkFree` also runs when a chunk unloads. The samplers read
  // this chunk's just-baked, chunk-LOCAL overlays (lock-free), so the hook
  // needn't know any tile layout. NULL `chunkBake` = no per-chunk subsystem.
  void *(*chunkBake)(void *user, BrushChunkCoord coord, Vector3 origin,
                     float size, const struct BrushChunkSamplers *samplers);
  void (*chunkFree)(void *user, void *handle);
  void *chunkUser;
} BrushWorldConfig;

typedef struct BrushWorld BrushWorld;

// A drawable active chunk, for a per-chunk subsystem's per-frame draw (foliage
// gather). `handle` is what chunkBake returned; `maxY` is the chunk's highest
// terrain point (for horizon culling). Positions are RENDER space.
typedef struct BrushWorldChunkView {
  BrushChunkCoord coord;
  Vector3 origin; // render-space chunk origin (XZ)
  float size;
  float maxY;
  void *handle;
} BrushWorldChunkView;

// Fill `out` (capacity `max`) with every drawable chunk that has a subsystem
// handle. Returns the count. Call per frame from the scene-draw callback.
int BrushWorldGetActiveChunks(const BrushWorld *w, BrushWorldChunkView *out,
                              int max);

// Mark EVERY resident chunk dirty so it re-bakes (mesh + splat + the chunkBake
// subsystem hook). For live editor edits that change per-chunk generation —
// e.g. re-scattering foliage after a layer's density/model changes. A few ms.
void BrushWorldRebakeAll(BrushWorld *w);

// --- Foliage density mask (paint where each foliage layer grows) ------------
// Sparse per-layer density MULTIPLIER painted over the terrain (0 = none,
// 1 = base, up to ~3 = thickened), riding the sculpt/splat tile machinery.
// `layer` is a foliage-layer slot (0..3 paintable). `erase` subtracts. Marks
// only the painted region dirty so those chunks re-scatter (localized, fast).
#define BRUSH_FOLIAGE_PAINT_MAX 4
#define BRUSH_FOLIAGE_MAX_BOOST 3.0f // painted density ceiling (x base)
void BrushWorldPaintFoliage(BrushWorld *w, Vector3 center, float radius,
                            float strength, int layer, bool erase);

// Create the world, start the worker, and block until the initial ring around
// `spawn` is fully resident (avoids terrain pop-in on the first frame).
BrushWorld *BrushWorldCreate(BrushWorldConfig cfg, Vector3 spawn);
void BrushWorldDestroy(BrushWorld *w);

// Per-frame: recompute the desired ring around `focus`, queue new chunks,
// finalize a bounded number of CPU-ready chunks (GPU upload + collider), and
// release chunks outside the unload ring. Cheap when the focus hasn't changed
// chunk. Call once per frame (or fixed step) before submitting.
void BrushWorldUpdate(BrushWorld *w, Vector3 focus);

// Submit the visible (frustum-culled) terrain chunks to the render layers
// (opaque + shadow caster). Call inside the game's draw, before
// BrushRenderExecute.
void BrushWorldSubmit(BrushWorld *w, Camera3D camera);

// Ground height at a world XZ: bilinear sample of the resident chunk's
// heightmap — the exact surface the terrain renders, so things placed on it
// sit on what they see. Returns 0 if that chunk isn't resident yet.
float BrushWorldGroundHeight(BrushWorld *w, float wx, float wz);

// Which chunk a world position falls in (floor division, negative-safe).
BrushChunkCoord BrushWorldChunkAt(const BrushWorld *w, Vector3 worldPos);

// --- Terrain sculpting (the namesake feature) --------------------------------
// A sparse DELTA overlay on top of heightFn: final height = heightFn + delta.
// Deltas live in tiles allocated on first touch (untouched world costs
// nothing — Terrain3D's region idea), sampled on the heightmap grid, and
// composed during the chunk bake — so the mesh, the Jolt collider, and
// gameplay ground queries all update together through the normal
// dirty-chunk rebake. Sculpting never re-calls heightFn for painted areas.
//
// Ops (one brush core, Terrain3D's operation model):
//   ADD      delta += strength * falloff      (negative strength lowers)
//   SMOOTH   total height relaxes toward its neighbourhood average;
//            strength 0..1 = blend per application
//   FLATTEN  total height blends toward targetY; strength 0..1
typedef enum {
  BRUSH_SCULPT_ADD = 0,
  BRUSH_SCULPT_SMOOTH,
  BRUSH_SCULPT_FLATTEN,
} BrushSculptOp;

typedef struct BrushConstraints {
  bool  checkSlope;                 // gate on surface steepness
  float minCosSlope, maxCosSlope;   // cos(angle): flat=1, vertical=0
  bool  checkHeight;                // gate on absolute Y
  float minHeight, maxHeight;
  int   targetLayer;                // -1 = any; else only where this layer dominant
} BrushConstraints;

// Apply one brush dab at `center` (world XZ; radius in metres, smoothstep
// falloff). Touched chunks are marked for rebake automatically; they keep
// drawing their old mesh until the worker delivers the new one.
void BrushWorldSculpt(BrushWorld *w, BrushSculptOp op, Vector3 center,
                      float radius, float strength, float targetY);
void BrushWorldSculptC(BrushWorld *w, BrushSculptOp op, Vector3 center,
                       float radius, float strength, float targetY,
                       const BrushConstraints *c);

// True if any sculpt data exists (used to decide whether to save).
bool BrushWorldSculptAny(const BrushWorld *w);

// Persist / restore the whole overlay (binary; safe to call with no data —
// Load with a missing file returns false and leaves the overlay empty).
bool BrushWorldSculptSave(BrushWorld *w, const char *path);
bool BrushWorldSculptLoad(BrushWorld *w, const char *path);

// Undo support: snapshot every tile intersecting the world-XZ AABB into a
// malloc'd blob (caller frees; NULL if none). Restore overwrites those tiles
// from the blob and marks the area dirty. Snapshot at stroke start, push on
// an undo stack, Restore to undo. Covers BOTH overlays (height + paint):
// one cmd+Z stack for sculpting and painting.
unsigned char *BrushWorldSculptSnapshot(BrushWorld *w, Vector2 minXZ,
                                        Vector2 maxXZ, int *outSize);
void BrushWorldSculptRestore(BrushWorld *w, const unsigned char *blob,
                             int size);

// --- Terrain painting (docs/terrain-painting-plan.md) ------------------------
// A SECOND sparse overlay: RGBA8 layer weights on the same grid/tiles as
// the height deltas (weights always renormalise to sum 255; unpainted =
// full layer 0). Painting rides the same dirty-chunk rebake; each chunk
// bakes a small weight texture the lit shader blends the layers with.

// Configure the (up to 4) paintable layers — usually the resolved textures
// of material-library entries (see BrushSceneTerrainLayers). count 0
// disables splat rendering entirely (the zero-asset checker look). Any
// change re-bakes every resident chunk.
void BrushWorldSetLayers(BrushWorld *w,
                         const BrushTerrainLayer layers[BRUSH_TERRAIN_LAYERS],
                         int count);

// One paint dab: raise `layer` (0..3) inside the smoothstep brush and
// renormalise the others. strength ~0..1 per application (flow).
void BrushWorldPaint(BrushWorld *w, Vector3 center, float radius,
                     float strength, int layer);
void BrushWorldPaintC(BrushWorld *w, Vector3 center, float radius,
                      float strength, int layer, const BrushConstraints *c);

// --- Biomes (docs/biome-system-plan.md) --------------------------------------
// Set the climate field that generates the biome map. Copied in; marks every
// resident chunk dirty so they re-bake with the new field. climate->biomeCount
// 0 (or a NULL climate) disables biomes: one implicit biome, biomeAt -> {0,0,0}.
void BrushWorldSetBiomeClimate(BrushWorld *w, const BrushBiomeClimate *climate);

// Auto-slope mask: terrain steeper than `startDeg` blends toward `layer`
// (fully by `endDeg`) BENEATH the painted weights — pure shader, no data.
// layer -1 disables (default).
void BrushWorldSetAutoSlope(BrushWorld *w, int layer, float startDeg,
                            float endDeg);

// Per-layer auto-height bands beneath the paint: layer i fades in between
// start[i] and full[i] (full>start = upward/snowline, full<start =
// downward/shoreline). on[i]=0 disables that layer's band. Arrays are
// BRUSH_TERRAIN_LAYERS long.
void BrushWorldSetLayerHeights(BrushWorld *w, const int *on,
                               const float *start, const float *full);

// Dominant painted layer at a world XZ (nearest grid sample), for
// footsteps/particles. -1 when no layers are configured.
int BrushWorldSurfaceAt(BrushWorld *w, float wx, float wz);

// --- Spline roads (LIVE / non-destructive) -----------------------------------
// A road is a Catmull-Rom spline (through `points`, XZ path / per-node Y = road
// surface) that flattens the terrain within `width` (easing back over `fade`)
// and paints `layerSlot` across the corridor (slot < 0 = carve shape only).
// Unlike a stamp, roads are re-evaluated DURING the chunk bake — so editing one
// and re-submitting via BrushWorldSetRoads updates the mesh, collider, ground
// query, and foot-IK automatically through the normal dirty-chunk rebake, with
// no destructive tile writes. Roads compose ON TOP of heightFn + manual sculpt.
#define BRUSH_ROAD_MAX_POINTS 32
#define BRUSH_WORLD_MAX_ROADS 32
typedef struct BrushWorldRoad {
  Vector3 points[BRUSH_ROAD_MAX_POINTS];
  int pointCount;
  float width;     // full-strength corridor width
  float fade;      // HEIGHT shoulder: terrain eases back over this margin
  float paintFade; // TEXTURE edge: 0 = hard (paving), >0 = feathered (dirt)
  int layerSlot;   // splat slot to paint; -1 = carve shape only
} BrushWorldRoad;

// Replace the world's live road set (copied in). Re-caches the spline polylines
// and marks every chunk overlapping the OLD or NEW roads dirty so they rebake
// with the change. Call whenever roads change (editor edit) or after loading
// them from a scene. count 0 clears all roads.
void BrushWorldSetRoads(BrushWorld *w, const BrushWorldRoad *roads, int count);

// Set the shared road SURFACE material (albedo/normal/tile) — composited over
// the terrain along the road corridor, independent of the 4 terrain layers so
// it never bleeds into the layer mix. NULL / id-0 albedo clears it (roads then
// carve height only). Marks road-overlapping chunks dirty to rebake the mask.
void BrushWorldSetRoadMaterial(BrushWorld *w, const BrushTerrainLayer *mat);

// Flatten a road spline to a world-space polyline at ~gridStep spacing (phantom
// endpoints so it reaches both ends). Returns a MemAlloc'd array of `*outN`
// points (caller MemFree) or NULL if count < 2. SHARED by the bake and the
// editor preview so they sample the corridor identically.
Vector3 *BrushWorldRoadPolyline(const BrushWorld *w, const Vector3 *points,
                                int count, int *outN);

float BrushWorldGetGridStep(const BrushWorld *w);

#ifdef __cplusplus
}
#endif

#endif // B_WORLD_H
