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

#include "b_physics.h"

#include <raylib.h>
#include <stdbool.h>

// Integer chunk index. The world is addressed as a ChunkCoord + a local offset
// in [0, chunkSize). int32 covers a practically unbounded index range; the real
// limit is float precision of the local offset (handled by the rebase seam).
typedef struct BrushChunkCoord {
  int x, z;
} BrushChunkCoord;

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

  // Per-chunk terrain collider registered with this physics world (NULL -> no
  // terrain collision). Colliders stream in/out with their chunks.
  BrushPhysics *physics;

  // Optional tiling ground texture (0 id -> flat vertex-colour terrain, shaded
  // green lowland / grey rock by slope). Tiles in WORLD space so it doesn't
  // reset per chunk.
  Texture2D groundTex;
  float texMetresPerTile; // world metres per texture repeat (0 -> 4)
} BrushWorldConfig;

typedef struct BrushWorld BrushWorld;

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

#endif // B_WORLD_H
