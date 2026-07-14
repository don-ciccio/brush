/*******************************************************************************************
 *   b_world.c - Chunk-streamed open world (see b_world.h)
 *
 *   Ported and generalized from the donor codebase's world.c: the terrain
 *   surface is a game-supplied function instead of a hardwired noise field,
 *   chunk size / radius / resolution are config, and the render path submits
 *   through the engine's layer stack (lit shader + shadows) rather than a
 *   bespoke terrain shader. Road carving, building flatten boxes, and foliage
 *   scatter stay in the game (they live inside heightFn or a future system).
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_world.h"
#include "b_render.h"

#include <math.h>
#include <pthread.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Chunk progression. The worker only advances QUEUED -> GENERATING ->
// CPU_READY (pure CPU). The main thread does CPU_READY -> ACTIVE (GPU upload +
// collider) and ACTIVE -> EMPTY (free).
typedef enum {
  CHUNK_EMPTY = 0,
  CHUNK_QUEUED,
  CHUNK_GENERATING,
  CHUNK_CPU_READY,
  CHUNK_ACTIVE,
} ChunkState;

typedef struct WorldChunk {
  BrushChunkCoord coord;
  // Cross-thread handoff. The worker publishes CPU_READY with a seq-cst store
  // after fully writing the CPU data; the main thread polls it lock-free, which
  // also makes those writes visible (publish/acquire). Atomic, not a plain
  // enum: raylib's own loading-thread example shipped a bug where a non-atomic
  // flag let the compiler optimize the check away (raylib #827).
  _Atomic ChunkState state;
  bool valid; // CPU build succeeded

  float *heightmap; // (hmRes+2)^2 samples incl. a 1-texel apron, row-major
  Mesh mesh;        // displaced terrain, baked CPU-side, uploaded main-thread
  bool meshUploaded;
  float maxY;

  JPH_BodyID terrainBody;  // Jolt static-mesh collider, or BRUSH_BODY_INVALID
  JPH_Shape *pendingShape; // collider cooked on the WORKER (BVH build is the
                           // expensive half of AddStaticMesh); the main-thread
                           // finalize only creates the body from it

  bool needsRebake; // sculpted while mid-build: re-enqueue after finalize
                    // (main-thread only)

  // LOD: `lod` is what chunk->mesh was baked at (worker reads it in the
  // bake); `desiredLod` is the ring the streaming update wants (main thread
  // writes). They differ across a ring crossing -> rebake. A LOD change
  // resizes the mesh, so the worker builds into `pendingMesh` (fresh CPU
  // arrays) and finalize swaps + re-uploads (UpdateMeshBuffer can't resize).
  int lod, desiredLod;
  int gridTriCount; // grid triangles (excludes skirt) — collider cooks these
  Mesh pendingMesh; // resized bake awaiting the main-thread GPU swap
  bool hasPendingMesh;

  // Terrain paint: per-chunk layer weights (hmRes^2 RGBA8), composed from
  // the weight tiles on the WORKER, uploaded as a texture at finalize.
  unsigned char *splatPixels;
  bool splatValid;    // pixels filled this bake (layers configured)
  Texture2D splatTex; // GPU copy (kept across slot reuse, like the mesh)
  // Road coverage (independent of the terrain splat): hmRes^2 R8, 0..255 = how
  // much the road surface covers this texel. Composited OVER the terrain blend
  // with its OWN material, so a road never leaks into the terrain layer mix.
  unsigned char *roadPixels;
  bool roadValid;
  Texture2D roadTex;
  unsigned char *maskPixels; // hmRes^2 * 4 foliage density (composed under lock,
                             // then read lock-free by the scatter samplers)
  bool maskValid;

  // Per-chunk subsystem handle (instanced foliage). `pendingHandle` is baked on
  // the worker and swapped into `handle` at finalize, exactly like pendingMesh
  // (foliage-plan.md §4). `handle` is immutable between finalizes and the ONLY
  // thing the per-frame draw reads.
  void *handle;
  void *pendingHandle;
} WorldChunk;

// Sparse sculpt tile: `tileRes`^2 delta samples on the heightmap grid.
// Tile (tx,tz) canonically owns global grid indices [t*tileRes,
// (t+1)*tileRes - 1] per axis — chunk-edge samples belong to exactly one
// tile, so writers and readers never disagree across borders.
typedef struct SculptTile {
  int tx, tz;
  float *d; // tileRes * tileRes deltas (metres)
} SculptTile;

// Sparse paint tile: same grid, same canonical ownership; RGBA8 layer
// weights per sample (sum 255; absent tile = implicit full layer 0).
// Sparse foliage density-mask tile: same grid/ownership as sculpt/splat; RGBA8
// per sample = painted density multiplier for foliage layers 0..3. Byte value
// b maps to multiplier (b/255)*MAX_BOOST; the neutral "1x base" is byte ~85.
#define FOLIAGE_MASK_NEUTRAL 85 // ~= 255 / BRUSH_FOLIAGE_MAX_BOOST -> M = 1.0
typedef struct FoliageMaskTile {
  int tx, tz;
  unsigned char *m; // tileRes * tileRes * 4
} FoliageMaskTile;

typedef struct SplatTile {
  int tx, tz;
  unsigned char *w; // tileRes * tileRes * 4 weights
} SplatTile;

struct BrushChunkJobQueue; // fwd

struct BrushWorld {
  BrushWorldConfig cfg;
  BrushChunkCoord center;
  BrushChunkCoord origin; // rebase seam origin (identity today)

  int hmTexRes;  // hmRes + 2 (apron)
  int unloadRadius;
  int maxChunks;
  int lodRadii[3];     // 0 in [0] -> single full-res ring (LOD disabled)
  bool lodEnabled;
  int collisionRadius; // chunks within this Chebyshev radius get colliders

  WorldChunk *chunks;
  int chunkCount;

  Material material; // shared across every chunk mesh
  bool ownsGroundTex;

  // Sculpt overlay: sparse delta tiles on the heightmap grid. The worker
  // composes them during chunk bakes while the main thread writes brush
  // strokes — the mutex covers the tile array and its data.
  SculptTile *tiles;
  int tileCount, tileCap;
  SplatTile *wtiles; // paint weights (same mutex/keying as height tiles)
  int wtileCount, wtileCap;
  FoliageMaskTile *fmtiles; // foliage density paint (same mutex/keying)
  int fmtileCount, fmtileCap;
  int tileRes;         // hmRes - 1 samples per tile side
  float gridStep;      // metres between grid samples (chunkSize / (hmRes-1))
  pthread_mutex_t sculptMutex;

  BrushTerrainLayer layers[BRUSH_TERRAIN_LAYERS];
  int layerCount;
  int autoSlopeLayer; // -1 = off
  float autoSlopeStart, autoSlopeEnd; // degrees
  int   layerHeightOn[BRUSH_TERRAIN_LAYERS];
  float layerHeightStart[BRUSH_TERRAIN_LAYERS];
  float layerHeightFull[BRUSH_TERRAIN_LAYERS];

  // Live roads: evaluated during the bake (BuildCpu), guarded by sculptMutex.
  // Polylines + world AABBs (expanded by width/2+fade) are cached per road so
  // the per-chunk bake only re-projects, and AABB-culls chunks that miss.
  BrushWorldRoad roads[BRUSH_WORLD_MAX_ROADS];
  int roadCount;
  Vector3 *roadPoly[BRUSH_WORLD_MAX_ROADS];
  int roadPolyN[BRUSH_WORLD_MAX_ROADS];
  float roadAABB[BRUSH_WORLD_MAX_ROADS][4]; // minX, minZ, maxX, maxZ (+ edge)
  BrushTerrainLayer roadLayer; // the road SURFACE material (own texture/tile)
  bool hasRoadLayer;           // a road material is set -> composite it

  struct BrushChunkJobQueue *jobs;
};

// ---- coordinates ----------------------------------------------------------

static BrushChunkCoord ChunkOf(const BrushWorld *w, float wx, float wz) {
  return (BrushChunkCoord){(int)floorf(wx / w->cfg.chunkSize),
                           (int)floorf(wz / w->cfg.chunkSize)};
}

BrushChunkCoord BrushWorldChunkAt(const BrushWorld *w, Vector3 p) {
  return ChunkOf(w, p.x, p.z);
}

static Vector3 ChunkOrigin(const BrushWorld *w, BrushChunkCoord c) {
  return (Vector3){(float)c.x * w->cfg.chunkSize, 0.0f,
                   (float)c.z * w->cfg.chunkSize};
}

static bool CoordEqual(BrushChunkCoord a, BrushChunkCoord b) {
  return a.x == b.x && a.z == b.z;
}

// THE rebase chokepoint: world position -> render-space. Identity today.
static Vector3 WorldToRender(const BrushWorld *w, Vector3 wp) {
  Vector3 o = ChunkOrigin(w, w->origin);
  return (Vector3){wp.x - o.x, wp.y, wp.z - o.z};
}

static float Height(const BrushWorld *w, float wx, float wz) {
  return w->cfg.heightFn ? w->cfg.heightFn(w->cfg.heightUser, wx, wz) : 0.0f;
}

// ---- LOD ------------------------------------------------------------------

// Mesh vertices per side for a LOD level: a grid subset of the full res, so
// every LOD vertex lands exactly on a full-res grid sample. Requires
// (meshRes-1) divisible by 2^lod (33 -> 33/17/9 for lod 0/1/2).
static int LodMeshRes(const BrushWorld *w, int lod) {
  int r = ((w->cfg.meshRes - 1) >> lod) + 1;
  return r < 2 ? 2 : r;
}

// Desired LOD for a chunk offset from centre, with 1-chunk hysteresis on the
// refine direction so a player hovering on a ring boundary doesn't thrash a
// row of chunks between resolutions (coarsening is cheap, so it's immediate).
static int DesiredLod(const BrushWorld *w, int dx, int dz, int cur) {
  if (!w->lodEnabled) return 0;
  int d = (abs(dx) > abs(dz)) ? abs(dx) : abs(dz);
  int b0 = w->lodRadii[0], b1 = w->lodRadii[1];
  int raw = (d <= b0) ? 0 : (d <= b1) ? 1 : 2;
  if (raw > cur) return raw;              // coarsen now
  if (raw < cur) {                        // refine only past a margin
    if (cur == 1 && d <= b0 - 1) return 0;
    if (cur == 2 && d <= b1 - 1) return 1;
    return cur;                           // dead zone: hold
  }
  return cur;
}

// ---- sculpt overlay (tile store; callers hold sculptMutex) -----------------

static long FloorDivL(long a, long b) { // negative-safe floor division
  long q = a / b, r = a % b;
  return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

static SculptTile *SculptFindTile(BrushWorld *w, int tx, int tz) {
  for (int i = 0; i < w->tileCount; i++)
    if (w->tiles[i].tx == tx && w->tiles[i].tz == tz) return &w->tiles[i];
  return NULL;
}

static SculptTile *SculptGetTile(BrushWorld *w, int tx, int tz, bool create) {
  SculptTile *t = SculptFindTile(w, tx, tz);
  if (t != NULL || !create) return t;
  if (w->tileCount == w->tileCap) {
    w->tileCap = (w->tileCap == 0) ? 16 : w->tileCap * 2;
    w->tiles = (SculptTile *)MemRealloc(w->tiles,
                                        sizeof(SculptTile) * w->tileCap);
  }
  t = &w->tiles[w->tileCount++];
  t->tx = tx;
  t->tz = tz;
  int n = w->tileRes * w->tileRes;
  t->d = (float *)MemAlloc(sizeof(float) * n); // MemAlloc zeroes
  return t;
}

// Delta at a global grid index (0 where no tile exists).
static float SculptDeltaAt(BrushWorld *w, long gx, long gz) {
  int tx = (int)FloorDivL(gx, w->tileRes), tz = (int)FloorDivL(gz, w->tileRes);
  SculptTile *t = SculptFindTile(w, tx, tz);
  if (t == NULL) return 0.0f;
  int lx = (int)(gx - (long)tx * w->tileRes);
  int lz = (int)(gz - (long)tz * w->tileRes);
  return t->d[lz * w->tileRes + lx];
}

static float *SculptSampleRW(BrushWorld *w, long gx, long gz) {
  int tx = (int)FloorDivL(gx, w->tileRes), tz = (int)FloorDivL(gz, w->tileRes);
  SculptTile *t = SculptGetTile(w, tx, tz, true);
  int lx = (int)(gx - (long)tx * w->tileRes);
  int lz = (int)(gz - (long)tz * w->tileRes);
  return &t->d[lz * w->tileRes + lx];
}

// ---- paint weight tiles (same locking + ownership as the height tiles) -----

static SplatTile *SplatFindTile(BrushWorld *w, int tx, int tz) {
  for (int i = 0; i < w->wtileCount; i++)
    if (w->wtiles[i].tx == tx && w->wtiles[i].tz == tz) return &w->wtiles[i];
  return NULL;
}

// New tiles start as the implicit default: full layer 0.
static void SplatTileDefault(BrushWorld *w, SplatTile *t) {
  int n = w->tileRes * w->tileRes;
  for (int i = 0; i < n; i++) {
    t->w[i * 4 + 0] = 255;
    t->w[i * 4 + 1] = 0;
    t->w[i * 4 + 2] = 0;
    t->w[i * 4 + 3] = 0;
  }
}

static SplatTile *SplatGetTile(BrushWorld *w, int tx, int tz, bool create) {
  SplatTile *t = SplatFindTile(w, tx, tz);
  if (t != NULL || !create) return t;
  if (w->wtileCount == w->wtileCap) {
    w->wtileCap = (w->wtileCap == 0) ? 16 : w->wtileCap * 2;
    w->wtiles = (SplatTile *)MemRealloc(w->wtiles,
                                        sizeof(SplatTile) * w->wtileCap);
  }
  t = &w->wtiles[w->wtileCount++];
  t->tx = tx;
  t->tz = tz;
  t->w = (unsigned char *)MemAlloc((unsigned int)(w->tileRes * w->tileRes * 4));
  SplatTileDefault(w, t);
  return t;
}

// Weights at a global grid index (default = full layer 0).
static void SplatWeightsAt(BrushWorld *w, long gx, long gz,
                           unsigned char out[4]) {
  int tx = (int)FloorDivL(gx, w->tileRes), tz = (int)FloorDivL(gz, w->tileRes);
  SplatTile *t = SplatFindTile(w, tx, tz);
  if (t == NULL) {
    out[0] = 255; out[1] = out[2] = out[3] = 0;
    return;
  }
  int lx = (int)(gx - (long)tx * w->tileRes);
  int lz = (int)(gz - (long)tz * w->tileRes);
  memcpy(out, &t->w[(lz * w->tileRes + lx) * 4], 4);
}

static unsigned char *SplatSampleRW(BrushWorld *w, long gx, long gz) {
  int tx = (int)FloorDivL(gx, w->tileRes), tz = (int)FloorDivL(gz, w->tileRes);
  SplatTile *t = SplatGetTile(w, tx, tz, true);
  int lx = (int)(gx - (long)tx * w->tileRes);
  int lz = (int)(gz - (long)tz * w->tileRes);
  return &t->w[(lz * w->tileRes + lx) * 4];
}

// ---- foliage density-mask tiles (same locking + ownership) -----------------

static FoliageMaskTile *FoliageMaskFind(BrushWorld *w, int tx, int tz) {
  for (int i = 0; i < w->fmtileCount; i++)
    if (w->fmtiles[i].tx == tx && w->fmtiles[i].tz == tz) return &w->fmtiles[i];
  return NULL;
}

static FoliageMaskTile *FoliageMaskGet(BrushWorld *w, int tx, int tz, bool create) {
  FoliageMaskTile *t = FoliageMaskFind(w, tx, tz);
  if (t != NULL || !create) return t;
  if (w->fmtileCount == w->fmtileCap) {
    w->fmtileCap = (w->fmtileCap == 0) ? 16 : w->fmtileCap * 2;
    w->fmtiles = (FoliageMaskTile *)MemRealloc(
        w->fmtiles, sizeof(FoliageMaskTile) * w->fmtileCap);
  }
  t = &w->fmtiles[w->fmtileCount++];
  t->tx = tx;
  t->tz = tz;
  int n = w->tileRes * w->tileRes;
  t->m = (unsigned char *)MemAlloc((unsigned int)(n * 4));
  memset(t->m, FOLIAGE_MASK_NEUTRAL, (unsigned int)(n * 4)); // = 1x base
  return t;
}

// Painted density multiplier (0..MAX_BOOST) for a foliage layer at a global
// grid index. Absent tile = neutral (M = 1 = base density).
static float FoliageMaskAt(BrushWorld *w, long gx, long gz, int layer) {
  if (layer < 0 || layer >= BRUSH_FOLIAGE_PAINT_MAX) return 1.0f;
  int tx = (int)FloorDivL(gx, w->tileRes), tz = (int)FloorDivL(gz, w->tileRes);
  FoliageMaskTile *t = FoliageMaskFind(w, tx, tz);
  unsigned char b = FOLIAGE_MASK_NEUTRAL;
  if (t != NULL) {
    int lx = (int)(gx - (long)tx * w->tileRes);
    int lz = (int)(gz - (long)tz * w->tileRes);
    b = t->m[(lz * w->tileRes + lx) * 4 + layer];
  }
  return (float)b / 255.0f * BRUSH_FOLIAGE_MAX_BOOST;
}

static unsigned char *FoliageMaskRW(BrushWorld *w, long gx, long gz) {
  int tx = (int)FloorDivL(gx, w->tileRes), tz = (int)FloorDivL(gz, w->tileRes);
  FoliageMaskTile *t = FoliageMaskGet(w, tx, tz, true);
  int lx = (int)(gx - (long)tx * w->tileRes);
  int lz = (int)(gz - (long)tz * w->tileRes);
  return &t->m[(lz * w->tileRes + lx) * 4];
}

// ---- terrain build (worker thread; no GL) ---------------------------------

// Bilinear sample of a chunk's heightmap at a world XZ — the exact surface the
// mesh interpolates between vertices, so placement/collision match the visuals.
static float SampleHeightmap(const BrushWorld *w, const WorldChunk *c, float wx,
                             float wz) {
  if (!c->heightmap) return 0.0f;
  Vector3 o = ChunkOrigin(w, c->coord);
  int hmRes = w->cfg.hmRes;
  float gx = (wx - o.x) / w->cfg.chunkSize * (float)(hmRes - 1);
  float gz = (wz - o.z) / w->cfg.chunkSize * (float)(hmRes - 1);
  gx = Clamp(gx, 0.0f, (float)(hmRes - 1));
  gz = Clamp(gz, 0.0f, (float)(hmRes - 1));
  int x0 = (int)gx, z0 = (int)gz;
  int x1 = (x0 + 1 < hmRes) ? x0 + 1 : x0;
  int z1 = (z0 + 1 < hmRes) ? z0 + 1 : z0;
  float fx = gx - (float)x0, fz = gz - (float)z0;
  // +1 to skip the 1-texel apron.
  int tr = w->hmTexRes;
  const float *hm = c->heightmap;
  float h00 = hm[(z0 + 1) * tr + (x0 + 1)];
  float h10 = hm[(z0 + 1) * tr + (x1 + 1)];
  float h01 = hm[(z1 + 1) * tr + (x0 + 1)];
  float h11 = hm[(z1 + 1) * tr + (x1 + 1)];
  return Lerp(Lerp(h00, h10, fx), Lerp(h01, h11, fx), fz);
}

// Apron-aware bilinear heightmap sample for the MESH BAKE: grid coordinates
// may reach one texel outside the chunk (-1 .. hmRes), which is exactly what
// the apron stores — edge normals difference into the neighbour chunk's
// surface without calling heightFn again. (SampleHeightmap above is the
// gameplay-query variant, clamped to the chunk interior.)
static float SampleHeightApron(const BrushWorld *w, const WorldChunk *c,
                               float wx, float wz) {
  Vector3 o = ChunkOrigin(w, c->coord);
  float step = w->cfg.chunkSize / (float)(w->cfg.hmRes - 1);
  float gx = Clamp((wx - o.x) / step, -1.0f, (float)w->cfg.hmRes);
  float gz = Clamp((wz - o.z) / step, -1.0f, (float)w->cfg.hmRes);
  int x0 = (int)floorf(gx), z0 = (int)floorf(gz);
  int x1 = (x0 + 1 <= w->cfg.hmRes) ? x0 + 1 : x0;
  int z1 = (z0 + 1 <= w->cfg.hmRes) ? z0 + 1 : z0;
  float fx = gx - (float)x0, fz = gz - (float)z0;
  int tr = w->hmTexRes;
  const float *hm = c->heightmap;
  float h00 = hm[(z0 + 1) * tr + (x0 + 1)]; // +1: apron offset
  float h10 = hm[(z0 + 1) * tr + (x1 + 1)];
  float h01 = hm[(z1 + 1) * tr + (x0 + 1)];
  float h11 = hm[(z1 + 1) * tr + (x1 + 1)];
  return Lerp(Lerp(h00, h10, fx), Lerp(h01, h11, fx), fz);
}

// Binds SampleHeightApron to one chunk so a per-chunk subsystem (foliage) can
// sample this chunk's just-baked surface without knowing the heightmap layout.
typedef struct { const BrushWorld *w; const WorldChunk *c; } ChunkHeightCtx;
static float ChunkHeightAt(void *ctx, float wx, float wz) {
  ChunkHeightCtx *h = (ChunkHeightCtx *)ctx;
  return SampleHeightApron(h->w, h->c, wx, wz);
}

// Nearest chunk-local grid index (hmRes^2, no apron) for a world XZ.
static int ChunkGridIndex(const BrushWorld *w, const WorldChunk *c, float wx,
                          float wz) {
  Vector3 o = ChunkOrigin(w, c->coord);
  float step = w->cfg.chunkSize / (float)(w->cfg.hmRes - 1);
  int ix = (int)((wx - o.x) / step + 0.5f);
  int iz = (int)((wz - o.z) / step + 0.5f);
  if (ix < 0) ix = 0; if (ix >= w->cfg.hmRes) ix = w->cfg.hmRes - 1;
  if (iz < 0) iz = 0; if (iz >= w->cfg.hmRes) iz = w->cfg.hmRes - 1;
  return iz * w->cfg.hmRes + ix;
}

static float ChunkDensityAt(void *ctx, float wx, float wz, int layer) {
  ChunkHeightCtx *h = (ChunkHeightCtx *)ctx;
  if (!h->c->maskValid || h->c->maskPixels == NULL || layer < 0 ||
      layer >= BRUSH_FOLIAGE_PAINT_MAX)
    return 1.0f; // unpainted -> base density
  int i = ChunkGridIndex(h->w, h->c, wx, wz);
  return (float)h->c->maskPixels[i * 4 + layer] / 255.0f * BRUSH_FOLIAGE_MAX_BOOST;
}

static void ChunkSplatAt(void *ctx, float wx, float wz, float out[4]) {
  ChunkHeightCtx *h = (ChunkHeightCtx *)ctx;
  if (!h->c->splatValid || h->c->splatPixels == NULL) {
    out[0] = 1.0f; out[1] = out[2] = out[3] = 0.0f; // full base layer
    return;
  }
  int i = ChunkGridIndex(h->w, h->c, wx, wz);
  const unsigned char *px = &h->c->splatPixels[i * 4];
  for (int k = 0; k < 4; k++) out[k] = (float)px[k] / 255.0f;
}

// Road surface coverage 0..1 at a world point (0 where there's no road). Lets
// foliage exclude itself from roads now that they're a separate mask, not a
// terrain splat slot. Baked before the foliage scatter in BuildCpu.
static float ChunkRoadAt(void *ctx, float wx, float wz) {
  ChunkHeightCtx *h = (ChunkHeightCtx *)ctx;
  if (!h->c->roadValid || h->c->roadPixels == NULL) return 0.0f;
  int i = ChunkGridIndex(h->w, h->c, wx, wz);
  return (float)h->c->roadPixels[i] / 255.0f;
}

// Vertex colour by slope: green lowland fading to grey rock on steep faces.
// Zero-asset default shading (the lit shader multiplies albedo by this).
static void SlopeColor(float ny, unsigned char *out) {
  float rock = 1.0f - Clamp((ny - 0.72f) / 0.20f, 0.0f, 1.0f); // steep -> 1
  Vector3 grass = {0.34f, 0.44f, 0.24f};
  Vector3 stone = {0.42f, 0.40f, 0.38f};
  Vector3 c = Vector3Lerp(grass, stone, rock);
  out[0] = (unsigned char)(c.x * 255.0f);
  out[1] = (unsigned char)(c.y * 255.0f);
  out[2] = (unsigned char)(c.z * 255.0f);
  out[3] = 255;
}

// Allocate CPU mesh arrays for `vc` vertices / `tc` triangles.
static void AllocMeshArrays(Mesh *m, int vc, int tc) {
  m->vertexCount = vc;
  m->triangleCount = tc;
  m->vertices = (float *)MemAlloc(sizeof(float) * vc * 3);
  m->normals = (float *)MemAlloc(sizeof(float) * vc * 3);
  m->texcoords = (float *)MemAlloc(sizeof(float) * vc * 2);
  m->colors = (unsigned char *)MemAlloc(sizeof(unsigned char) * vc * 4);
  m->indices = (unsigned short *)MemAlloc(sizeof(unsigned short) * tc * 3);
}

static void FreeMeshArrays(Mesh *m) {
  if (m->vertices) MemFree(m->vertices);
  if (m->normals) MemFree(m->normals);
  if (m->texcoords) MemFree(m->texcoords);
  if (m->colors) MemFree(m->colors);
  if (m->indices) MemFree(m->indices);
  *m = (Mesh){0};
}

// Build the displaced terrain mesh CPU-side. Vertices are LOCAL [0,size] with
// Y = Height(world); normals from central differences of the same field. Edge
// vertices land on exact shared world coords, so surface + normals are seamless
// across borders. Texcoords tile in WORLD space.
//
// Resolution follows chunk->lod (a grid subset of the full res). The border
// gets a downward SKIRT ring that hides the T-junction crack where a
// higher-detail ring meets a coarser one — no cross-chunk stitching. Grid
// triangles come first (chunk->gridTriCount, what the collider cooks), the
// skirt triangles after.
static void BuildMesh(const BrushWorld *w, WorldChunk *chunk) {
  const int R = LodMeshRes(w, chunk->lod);
  const float cell = w->cfg.chunkSize / (float)(R - 1);
  // Normal epsilon = one heightmap texel regardless of mesh cell, so even a
  // coarse LOD mesh lights by the true fine-surface gradient. Heights come
  // from the chunk's own apron'd heightmap (built just before this) — the
  // apron supplies the samples one texel OUTSIDE the chunk that edge normals
  // need, without re-calling the (expensive) heightFn.
  const float e = w->cfg.chunkSize / (float)(w->cfg.hmRes - 1);
  const float texScale = 1.0f / w->cfg.texMetresPerTile;
  const float skirtDepth = cell + 2.0f; // hangs safely below the surface
  Vector3 o = ChunkOrigin(w, chunk->coord);

  const int gridVC = R * R, skirtVC = 4 * R;
  const int VC = gridVC + skirtVC;
  const int gridTC = (R - 1) * (R - 1) * 2, skirtTC = 4 * (R - 1) * 2;
  const int TC = gridTC + skirtTC;

  // In-place fast path only when the uploaded buffers already match this
  // size (same-LOD rebakes: sculpt/paint). A LOD change resizes -> build
  // into pendingMesh; a fresh/mismatched slot buffer -> (re)alloc mesh.
  chunk->hasPendingMesh = false;
  Mesh *dst;
  if (chunk->meshUploaded) {
    if (chunk->mesh.vertexCount == VC) {
      dst = &chunk->mesh; // recycle GPU buffers via UpdateMeshBuffer
    } else {
      FreeMeshArrays(&chunk->pendingMesh);
      AllocMeshArrays(&chunk->pendingMesh, VC, TC);
      dst = &chunk->pendingMesh;
      chunk->hasPendingMesh = true;
    }
  } else {
    if (chunk->mesh.vertices == NULL || chunk->mesh.vertexCount != VC) {
      FreeMeshArrays(&chunk->mesh);
      AllocMeshArrays(&chunk->mesh, VC, TC);
    }
    dst = &chunk->mesh;
  }
  chunk->gridTriCount = gridTC;

  // Indices: grid quads, then skirt quads per edge.
  {
    unsigned short *idx = dst->indices;
    int t = 0;
    for (int j = 0; j < R - 1; j++)
      for (int i = 0; i < R - 1; i++) {
        unsigned short a = (unsigned short)(j * R + i);
        unsigned short b = (unsigned short)(j * R + i + 1);
        unsigned short c = (unsigned short)((j + 1) * R + i);
        unsigned short d = (unsigned short)((j + 1) * R + i + 1);
        idx[t++] = a; idx[t++] = c; idx[t++] = b;
        idx[t++] = b; idx[t++] = c; idx[t++] = d;
      }
    // Skirt vertex bases per edge: south, north, west, east.
    int sBase[4] = {gridVC, gridVC + R, gridVC + 2 * R, gridVC + 3 * R};
    for (int edge = 0; edge < 4; edge++) {
      for (int k = 0; k < R - 1; k++) {
        int gA, gB;
        if (edge == 0)      { gA = k; gB = k + 1; }                       // south j=0
        else if (edge == 1) { gA = (R - 1) * R + k; gB = gA + 1; }        // north
        else if (edge == 2) { gA = k * R; gB = (k + 1) * R; }             // west i=0
        else                { gA = k * R + (R - 1); gB = gA + R; }        // east
        unsigned short sA = (unsigned short)(sBase[edge] + k);
        unsigned short sB = (unsigned short)(sBase[edge] + k + 1);
        // Outward winding differs by edge side so front faces point away
        // from the chunk (skirts on both neighbours cover the crack).
        if (edge == 0 || edge == 3) {
          idx[t++] = (unsigned short)gA; idx[t++] = sA; idx[t++] = (unsigned short)gB;
          idx[t++] = (unsigned short)gB; idx[t++] = sA; idx[t++] = sB;
        } else {
          idx[t++] = (unsigned short)gA; idx[t++] = (unsigned short)gB; idx[t++] = sA;
          idx[t++] = (unsigned short)gB; idx[t++] = sB; idx[t++] = sA;
        }
      }
    }
  }

  // Grid vertices.
  float maxH = -1e30f;
  for (int j = 0; j < R; j++) {
    for (int i = 0; i < R; i++) {
      float lx = (float)i * cell, lz = (float)j * cell;
      float wx = o.x + lx, wz = o.z + lz;
      float h = SampleHeightApron(w, chunk, wx, wz);
      float hL = SampleHeightApron(w, chunk, wx - e, wz);
      float hR = SampleHeightApron(w, chunk, wx + e, wz);
      float hD = SampleHeightApron(w, chunk, wx, wz - e);
      float hU = SampleHeightApron(w, chunk, wx, wz + e);
      Vector3 n = Vector3Normalize((Vector3){hL - hR, 2.0f * e, hD - hU});
      int v = j * R + i;
      dst->vertices[v * 3 + 0] = lx;
      dst->vertices[v * 3 + 1] = h;
      dst->vertices[v * 3 + 2] = lz;
      dst->normals[v * 3 + 0] = n.x;
      dst->normals[v * 3 + 1] = n.y;
      dst->normals[v * 3 + 2] = n.z;
      dst->texcoords[v * 2 + 0] = wx * texScale;
      dst->texcoords[v * 2 + 1] = wz * texScale;
      if (w->cfg.groundTex.id > 0) {
        unsigned char *col = &dst->colors[v * 4];
        col[0] = col[1] = col[2] = col[3] = 255;
      } else {
        SlopeColor(n.y, &dst->colors[v * 4]);
      }
      if (h > maxH) maxH = h;
    }
  }

  // Skirt vertices: copy each border vertex, drop Y (normal/uv/colour kept
  // so lighting doesn't crease at the skirt's top edge).
  int sBase[4] = {gridVC, gridVC + R, gridVC + 2 * R, gridVC + 3 * R};
  for (int edge = 0; edge < 4; edge++) {
    for (int k = 0; k < R; k++) {
      int g;
      if (edge == 0)      g = k;                    // south
      else if (edge == 1) g = (R - 1) * R + k;      // north
      else if (edge == 2) g = k * R;                // west
      else                g = k * R + (R - 1);      // east
      int s = sBase[edge] + k;
      dst->vertices[s * 3 + 0] = dst->vertices[g * 3 + 0];
      dst->vertices[s * 3 + 1] = dst->vertices[g * 3 + 1] - skirtDepth;
      dst->vertices[s * 3 + 2] = dst->vertices[g * 3 + 2];
      dst->normals[s * 3 + 0] = dst->normals[g * 3 + 0];
      dst->normals[s * 3 + 1] = dst->normals[g * 3 + 1];
      dst->normals[s * 3 + 2] = dst->normals[g * 3 + 2];
      dst->texcoords[s * 2 + 0] = dst->texcoords[g * 2 + 0];
      dst->texcoords[s * 2 + 1] = dst->texcoords[g * 2 + 1];
      memcpy(&dst->colors[s * 4], &dst->colors[g * 4], 4);
    }
  }
  chunk->maxY = maxH;
}

// Live-road bake hooks (defined with the road section below). Both take world
// XZ and are called under sculptMutex from BuildCpu.
static float RoadHeightAt(const BrushWorld *w, float wx, float wz, float h);
static unsigned char RoadCoverageAt(const BrushWorld *w, float wx, float wz);

static void BuildCpu(BrushWorld *w, WorldChunk *chunk) {
  Vector3 o = ChunkOrigin(w, chunk->coord);
  int tr = w->hmTexRes;
  if (!chunk->heightmap)
    chunk->heightmap = (float *)MemAlloc(sizeof(float) * tr * tr);
  float step = w->cfg.chunkSize / (float)(w->cfg.hmRes - 1);
  for (int z = 0; z < tr; z++) {
    for (int x = 0; x < tr; x++) {
      float wx = o.x + (float)(x - 1) * step; // -1: apron
      float wz = o.z + (float)(z - 1) * step;
      chunk->heightmap[z * tr + x] = Height(w, wx, wz);
    }
  }

  // Compose the sculpt overlay: final = heightFn + delta. Sampled on the
  // same grid the heightmap uses, so the mesh bake, collider, and gameplay
  // queries all pick it up from this one place.
  pthread_mutex_lock(&w->sculptMutex);
  if (w->tileCount > 0) {
    long baseGx = (long)chunk->coord.x * w->tileRes;
    long baseGz = (long)chunk->coord.z * w->tileRes;
    for (int z = 0; z < tr; z++)
      for (int x = 0; x < tr; x++)
        chunk->heightmap[z * tr + x] +=
            SculptDeltaAt(w, baseGx + (x - 1), baseGz + (z - 1));
  }
  // Live roads flatten the corridor toward their spline, composed AFTER manual
  // sculpt (a road wins over hand terrain). AABB-culled per road, so chunks
  // that miss every road pay almost nothing.
  if (w->roadCount > 0) {
    for (int z = 0; z < tr; z++)
      for (int x = 0; x < tr; x++) {
        float wx = o.x + (float)(x - 1) * step;
        float wz = o.z + (float)(z - 1) * step;
        chunk->heightmap[z * tr + x] =
            RoadHeightAt(w, wx, wz, chunk->heightmap[z * tr + x]);
      }
  }
  // Paint weights -> per-chunk pixels (hmRes^2, NO apron: chunk-edge grid
  // samples are canonically shared, so border texels match the neighbour's
  // and bilinear filtering is seamless with texel-centre UVs).
  chunk->splatValid = false;
  if (w->layerCount > 0) {
    int sr = w->cfg.hmRes;
    if (chunk->splatPixels == NULL)
      chunk->splatPixels = (unsigned char *)MemAlloc((unsigned int)(sr * sr * 4));
    long bGx = (long)chunk->coord.x * w->tileRes;
    long bGz = (long)chunk->coord.z * w->tileRes;
    for (int z = 0; z < sr; z++)
      for (int x = 0; x < sr; x++) {
        unsigned char *px = &chunk->splatPixels[(z * sr + x) * 4];
        SplatWeightsAt(w, bGx + x, bGz + z, px);
      }
    chunk->splatValid = true;
  }
  // Road coverage mask — baked SEPARATELY from the terrain splat so the road
  // surface is composited over the terrain with its own material and never
  // bleeds into the layer mix (or the auto-slope / auto-height rules).
  chunk->roadValid = false;
  if (w->roadCount > 0) { // bake coverage whenever roads exist (material-agnostic)
    int sr = w->cfg.hmRes;
    if (chunk->roadPixels == NULL)
      chunk->roadPixels = (unsigned char *)MemAlloc((unsigned int)(sr * sr));
    long bGx = (long)chunk->coord.x * w->tileRes;
    long bGz = (long)chunk->coord.z * w->tileRes;
    for (int z = 0; z < sr; z++)
      for (int x = 0; x < sr; x++)
        chunk->roadPixels[z * sr + x] = RoadCoverageAt(
            w, (float)(bGx + x) * w->gridStep, (float)(bGz + z) * w->gridStep);
    chunk->roadValid = true;
  }
  // Foliage density mask -> per-chunk pixels (only where painting exists, so an
  // unpainted world pays nothing). Composed under the lock like splat; the
  // scatter samplers then read it lock-free. Same canonical-ownership grid.
  chunk->maskValid = false;
  if (w->fmtileCount > 0) {
    int sr = w->cfg.hmRes;
    if (chunk->maskPixels == NULL)
      chunk->maskPixels = (unsigned char *)MemAlloc((unsigned int)(sr * sr * 4));
    long bGx = (long)chunk->coord.x * w->tileRes;
    long bGz = (long)chunk->coord.z * w->tileRes;
    for (int z = 0; z < sr; z++)
      for (int x = 0; x < sr; x++) {
        unsigned char *px = &chunk->maskPixels[(z * sr + x) * 4];
        for (int L = 0; L < BRUSH_FOLIAGE_PAINT_MAX; L++) {
          float mm = FoliageMaskAt(w, bGx + x, bGz + z, L); // 0..MAX_BOOST
          int b = (int)(mm / BRUSH_FOLIAGE_MAX_BOOST * 255.0f + 0.5f);
          px[L] = (unsigned char)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
      }
    chunk->maskValid = true;
  }
  pthread_mutex_unlock(&w->sculptMutex);

  BuildMesh(w, chunk);

  // Cook the Jolt collider here on the worker: the BVH build is the
  // expensive half of collider creation and needs no physics-world access.
  // The main-thread finalize just wraps a body around it. ONLY full-res
  // (lod 0) chunks get colliders — nothing simulates in the distant rings,
  // so their BVH build and memory disappear. Cook GRID triangles only (the
  // skirt curtains hang below the surface and must not be walkable).
  if (w->cfg.physics && chunk->lod == 0) {
    Mesh src = chunk->hasPendingMesh ? chunk->pendingMesh : chunk->mesh;
    src.triangleCount = chunk->gridTriCount;
    Vector3 ro = WorldToRender(w, o);
    Matrix xf = MatrixTranslate(ro.x, 0.0f, ro.z);
    chunk->pendingShape = BrushPhysicsCookMeshShape(src, xf);
  }

  // Per-chunk subsystem (foliage): scatter into a pending handle from the
  // just-composed heightmap. Pure CPU. Freed/swapped on the main thread at
  // finalize; a stale pending (double rebake before finalize) is released here.
  if (w->cfg.chunkBake) {
    if (chunk->pendingHandle && w->cfg.chunkFree)
      w->cfg.chunkFree(w->cfg.chunkUser, chunk->pendingHandle);
    ChunkHeightCtx hc = {w, chunk};
    BrushChunkSamplers samplers = {ChunkHeightAt, ChunkDensityAt, ChunkSplatAt, ChunkRoadAt, &hc};
    chunk->pendingHandle = w->cfg.chunkBake(w->cfg.chunkUser, chunk->coord, o,
                                            w->cfg.chunkSize, &samplers);
  }

  chunk->valid = true;
}

// ---- worker + job queue ---------------------------------------------------

typedef struct BrushChunkJobQueue {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool started, stop;
  BrushWorld *owner;
  WorldChunk **pending; // ring of queued chunk pointers, size maxChunks
  int head, tail, count, cap;
} BrushChunkJobQueue;

static void *WorkerMain(void *arg) {
  BrushChunkJobQueue *q = (BrushChunkJobQueue *)arg;
  for (;;) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->stop) pthread_cond_wait(&q->cond, &q->mutex);
    if (q->stop && q->count == 0) {
      pthread_mutex_unlock(&q->mutex);
      break;
    }
    WorldChunk *chunk = q->pending[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_mutex_unlock(&q->mutex);

    atomic_store(&chunk->state, CHUNK_GENERATING);
    BuildCpu(q->owner, chunk);
    atomic_store(&chunk->state, CHUNK_CPU_READY);
  }
  return NULL;
}

static void QueueEnqueue(BrushChunkJobQueue *q, WorldChunk *chunk) {
  if (!q->started) { // inline fallback
    atomic_store(&chunk->state, CHUNK_GENERATING);
    BuildCpu(q->owner, chunk);
    atomic_store(&chunk->state, CHUNK_CPU_READY);
    return;
  }
  pthread_mutex_lock(&q->mutex);
  if (q->count < q->cap) {
    atomic_store(&chunk->state, CHUNK_QUEUED);
    q->pending[q->tail] = chunk;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
  } else {
    pthread_mutex_unlock(&q->mutex);
    atomic_store(&chunk->state, CHUNK_GENERATING);
    BuildCpu(q->owner, chunk);
    atomic_store(&chunk->state, CHUNK_CPU_READY);
  }
}

// ---- chunk slots ----------------------------------------------------------

static WorldChunk *ChunkResident(BrushWorld *w, BrushChunkCoord c) {
  for (int i = 0; i < w->chunkCount; i++)
    if (w->chunks[i].state != CHUNK_EMPTY && CoordEqual(w->chunks[i].coord, c))
      return &w->chunks[i];
  return NULL;
}

static WorldChunk *AcquireSlot(BrushWorld *w, BrushChunkCoord c) {
  for (int i = 0; i < w->chunkCount; i++) {
    if (w->chunks[i].state == CHUNK_EMPTY) {
      WorldChunk *k = &w->chunks[i];
      float *hm = k->heightmap; // keep the allocated buffers for reuse
      Mesh mesh = k->mesh;
      bool up = k->meshUploaded;
      unsigned char *sp = k->splatPixels;
      Texture2D st = k->splatTex;
      unsigned char *rp = k->roadPixels;
      Texture2D rtx = k->roadTex;
      memset(k, 0, sizeof(*k));
      k->coord = c;
      k->heightmap = hm;
      k->mesh = mesh;
      k->meshUploaded = up;
      k->splatPixels = sp;
      k->splatTex = st;
      k->roadPixels = rp;
      k->roadTex = rtx;
      k->terrainBody = BRUSH_BODY_INVALID;
      return k;
    }
  }
  if (w->chunkCount < w->maxChunks) {
    WorldChunk *k = &w->chunks[w->chunkCount++];
    memset(k, 0, sizeof(*k));
    k->coord = c;
    k->terrainBody = BRUSH_BODY_INVALID;
    return k;
  }
  return NULL;
}

// Main-thread finalize: CPU_READY -> ACTIVE (GPU upload + collider).
static bool FinalizeChunk(BrushWorld *w, WorldChunk *chunk) {
  if (atomic_load(&chunk->state) != CHUNK_CPU_READY) return false;
  if (!chunk->valid) return false;

  if (IsWindowReady()) {
    if (chunk->hasPendingMesh) {
      // LOD resize: swap the new-sized bake in for the old GPU mesh.
      // UnloadMesh frees the OLD GPU buffers AND the old CPU arrays.
      if (chunk->meshUploaded) UnloadMesh(chunk->mesh);
      else FreeMeshArrays(&chunk->mesh);
      chunk->mesh = chunk->pendingMesh;
      chunk->pendingMesh = (Mesh){0};
      chunk->hasPendingMesh = false;
      UploadMesh(&chunk->mesh, false);
      chunk->meshUploaded = true;
    } else if (!chunk->meshUploaded) {
      UploadMesh(&chunk->mesh, false);
      chunk->meshUploaded = true;
    } else {
      // Same-size rebake (sculpt/paint): recycle the GPU buffers in place.
      int vc = chunk->mesh.vertexCount;
      UpdateMeshBuffer(chunk->mesh, 0, chunk->mesh.vertices, sizeof(float) * vc * 3, 0);
      UpdateMeshBuffer(chunk->mesh, 1, chunk->mesh.texcoords, sizeof(float) * vc * 2, 0);
      UpdateMeshBuffer(chunk->mesh, 2, chunk->mesh.normals, sizeof(float) * vc * 3, 0);
      UpdateMeshBuffer(chunk->mesh, 3, chunk->mesh.colors, sizeof(unsigned char) * vc * 4, 0);
    }
  }

  if (IsWindowReady() && chunk->splatValid && chunk->splatPixels != NULL) {
    int sr = w->cfg.hmRes;
    if (chunk->splatTex.id == 0) {
      Image img = {.data = chunk->splatPixels,
                   .width = sr,
                   .height = sr,
                   .mipmaps = 1,
                   .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
      chunk->splatTex = LoadTextureFromImage(img); // copies the pixels
      SetTextureFilter(chunk->splatTex, TEXTURE_FILTER_BILINEAR);
      SetTextureWrap(chunk->splatTex, TEXTURE_WRAP_CLAMP);
    } else {
      UpdateTexture(chunk->splatTex, chunk->splatPixels);
    }
  }
  // Road coverage mask -> single-channel GPU texture (mirrors the splat upload).
  if (IsWindowReady() && chunk->roadValid && chunk->roadPixels != NULL) {
    int sr = w->cfg.hmRes;
    if (chunk->roadTex.id == 0) {
      Image img = {.data = chunk->roadPixels,
                   .width = sr,
                   .height = sr,
                   .mipmaps = 1,
                   .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE};
      chunk->roadTex = LoadTextureFromImage(img); // copies the pixels
      SetTextureFilter(chunk->roadTex, TEXTURE_FILTER_BILINEAR);
      SetTextureWrap(chunk->roadTex, TEXTURE_WRAP_CLAMP);
    } else {
      UpdateTexture(chunk->roadTex, chunk->roadPixels);
    }
  }

  if (w->cfg.physics && chunk->pendingShape) {
    if (chunk->terrainBody != BRUSH_BODY_INVALID) {
      BrushPhysicsRemoveBody(w->cfg.physics, chunk->terrainBody);
      chunk->terrainBody = BRUSH_BODY_INVALID;
    }
    char tag[48];
    snprintf(tag, sizeof(tag), "terrain_%d_%d", chunk->coord.x, chunk->coord.z);
    // Consumes pendingShape's reference (cooked on the worker in BuildCpu).
    chunk->terrainBody = BrushPhysicsAddStaticShape(
        w->cfg.physics, chunk->pendingShape, 0, tag);
    chunk->pendingShape = NULL;
  }

  // Publish the worker-baked subsystem handle (foliage): swap pending -> live
  // and release the previous one, in lockstep with the mesh swap above. The
  // old handle drew right up to this point; nothing reads it after the swap.
  if (chunk->pendingHandle) {
    if (chunk->handle && w->cfg.chunkFree)
      w->cfg.chunkFree(w->cfg.chunkUser, chunk->handle);
    chunk->handle = chunk->pendingHandle;
    chunk->pendingHandle = NULL;
  }

  atomic_store(&chunk->state, CHUNK_ACTIVE);

  // Sculpted or LOD-changed while this bake was in flight: straight back
  // around (adopt the desired LOD now that the worker is idle for it).
  if (chunk->needsRebake || chunk->desiredLod != chunk->lod) {
    chunk->needsRebake = false;
    chunk->lod = chunk->desiredLod;
    QueueEnqueue(w->jobs, chunk);
  }
  return true;
}

static void ReleaseChunk(BrushWorld *w, WorldChunk *chunk) {
  if (w->cfg.physics && chunk->terrainBody != BRUSH_BODY_INVALID) {
    BrushPhysicsRemoveBody(w->cfg.physics, chunk->terrainBody);
    chunk->terrainBody = BRUSH_BODY_INVALID;
  }
  // A chunk unloaded while CPU_READY still owns its worker-cooked shape.
  if (chunk->pendingShape) {
    BrushPhysicsReleaseShape(chunk->pendingShape);
    chunk->pendingShape = NULL;
  }
  // A resized bake in flight owns fresh CPU arrays not yet swapped in.
  if (chunk->hasPendingMesh) {
    FreeMeshArrays(&chunk->pendingMesh);
    chunk->hasPendingMesh = false;
  }
  // Release the subsystem handles (foliage re-scatters on slot reuse).
  if (w->cfg.chunkFree) {
    if (chunk->pendingHandle) {
      w->cfg.chunkFree(w->cfg.chunkUser, chunk->pendingHandle);
      chunk->pendingHandle = NULL;
    }
    if (chunk->handle) {
      w->cfg.chunkFree(w->cfg.chunkUser, chunk->handle);
      chunk->handle = NULL;
    }
  }
  // Keep heightmap + mesh buffers allocated for slot reuse.
  chunk->state = CHUNK_EMPTY;
}

static void DestroyChunkBuffers(WorldChunk *chunk) {
  if (chunk->hasPendingMesh) {
    FreeMeshArrays(&chunk->pendingMesh);
    chunk->hasPendingMesh = false;
  }
  if (chunk->splatTex.id != 0) {
    UnloadTexture(chunk->splatTex);
    chunk->splatTex = (Texture2D){0};
  }
  if (chunk->splatPixels) {
    MemFree(chunk->splatPixels);
    chunk->splatPixels = NULL;
  }
  if (chunk->roadTex.id != 0) {
    UnloadTexture(chunk->roadTex);
    chunk->roadTex = (Texture2D){0};
  }
  if (chunk->roadPixels) {
    MemFree(chunk->roadPixels);
    chunk->roadPixels = NULL;
  }
  if (chunk->maskPixels) {
    MemFree(chunk->maskPixels);
    chunk->maskPixels = NULL;
  }
  if (chunk->meshUploaded) {
    UnloadMesh(chunk->mesh);
    chunk->meshUploaded = false;
  } else {
    if (chunk->mesh.vertices) MemFree(chunk->mesh.vertices);
    if (chunk->mesh.normals) MemFree(chunk->mesh.normals);
    if (chunk->mesh.texcoords) MemFree(chunk->mesh.texcoords);
    if (chunk->mesh.colors) MemFree(chunk->mesh.colors);
    if (chunk->mesh.indices) MemFree(chunk->mesh.indices);
  }
  chunk->mesh = (Mesh){0};
  if (chunk->heightmap) {
    MemFree(chunk->heightmap);
    chunk->heightmap = NULL;
  }
}

// ---- lifecycle ------------------------------------------------------------

BrushWorld *BrushWorldCreate(BrushWorldConfig cfg, Vector3 spawn) {
  if (cfg.chunkSize <= 0.0f) cfg.chunkSize = 64.0f;
  if (cfg.loadRadius <= 0) cfg.loadRadius = 4;
  if (cfg.meshRes <= 1) cfg.meshRes = 33;
  if (cfg.hmRes <= 1) cfg.hmRes = 65;
  if (cfg.texMetresPerTile <= 0.0f) cfg.texMetresPerTile = 4.0f;

  // LOD rings: valid only when strictly increasing and non-zero. The outer
  // ring becomes the residency radius (LOD extends view distance without
  // paying its triangle cost).
  bool lodEnabled = (cfg.lodRadii[0] > 0 && cfg.lodRadii[1] > cfg.lodRadii[0] &&
                     cfg.lodRadii[2] > cfg.lodRadii[1]);
  if (lodEnabled) cfg.loadRadius = cfg.lodRadii[2];

  BrushWorld *w = (BrushWorld *)MemAlloc(sizeof(BrushWorld));
  memset(w, 0, sizeof(*w));
  w->cfg = cfg;
  w->lodEnabled = lodEnabled;
  w->lodRadii[0] = cfg.lodRadii[0];
  w->lodRadii[1] = cfg.lodRadii[1];
  w->lodRadii[2] = cfg.lodRadii[2];
  // Colliders only on the full-res ring (== lod 0); nothing simulates farther.
  w->collisionRadius = lodEnabled ? cfg.lodRadii[0] : cfg.loadRadius;
  w->hmTexRes = cfg.hmRes + 2;
  w->tileRes = cfg.hmRes - 1;
  w->gridStep = cfg.chunkSize / (float)(cfg.hmRes - 1);
  pthread_mutex_init(&w->sculptMutex, NULL);
  w->autoSlopeLayer = -1;
  w->autoSlopeStart = 25.0f;
  w->autoSlopeEnd = 45.0f;
  // Splat layers BEFORE the initial ring builds, so its chunks bake the splat
  // texture on the first pass (a post-create SetLayers would dirty them all and
  // pop the texture in ~1s later).
  if (cfg.layerCount > 0) {
    int lc = cfg.layerCount > BRUSH_TERRAIN_LAYERS ? BRUSH_TERRAIN_LAYERS
                                                   : cfg.layerCount;
    memcpy(w->layers, cfg.layers, sizeof(BrushTerrainLayer) * (size_t)lc);
    w->layerCount = lc;
  }
  w->unloadRadius = cfg.loadRadius + 1;
  w->origin = (BrushChunkCoord){0, 0};
  w->center = ChunkOf(w, spawn.x, spawn.z);

  // Slot ceiling: the unload ring plus slack for chunks mid-transition.
  int side = 2 * w->unloadRadius + 1;
  w->maxChunks = side * side + 16;
  w->chunks = (WorldChunk *)MemAlloc(sizeof(WorldChunk) * w->maxChunks);
  memset(w->chunks, 0, sizeof(WorldChunk) * w->maxChunks);

  // Shared terrain material on the engine lit shader (lighting + shadows).
  w->material = LoadMaterialDefault();
  w->material.shader = BrushGetLitShader();
  if (cfg.groundTex.id > 0) {
    w->material.maps[MATERIAL_MAP_DIFFUSE].texture = cfg.groundTex;
    w->material.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  }

  // Worker.
  w->jobs = (BrushChunkJobQueue *)MemAlloc(sizeof(BrushChunkJobQueue));
  memset(w->jobs, 0, sizeof(BrushChunkJobQueue));
  w->jobs->owner = w;
  w->jobs->cap = w->maxChunks;
  w->jobs->pending =
      (WorldChunk **)MemAlloc(sizeof(WorldChunk *) * w->maxChunks);
  pthread_mutex_init(&w->jobs->mutex, NULL);
  pthread_cond_init(&w->jobs->cond, NULL);
  w->jobs->started =
      (pthread_create(&w->jobs->thread, NULL, WorkerMain, w->jobs) == 0);
  if (!w->jobs->started)
    TraceLog(LOG_WARNING, "BRUSH world: worker failed; building inline");

  // Queue the initial ring and block until it's all resident (no pop-in).
  double t0 = GetTime();
  BrushWorldUpdate(w, spawn);
  for (;;) {
    int pending = 0;
    for (int i = 0; i < w->chunkCount; i++) {
      ChunkState s = atomic_load(&w->chunks[i].state);
      if (s == CHUNK_CPU_READY) FinalizeChunk(w, &w->chunks[i]);
      else if (s == CHUNK_QUEUED || s == CHUNK_GENERATING) pending++;
    }
    if (pending == 0) break;
  }
  TraceLog(LOG_INFO,
           "BRUSH world: ready at chunk (%d,%d), %d chunks, %.0fm each "
           "(initial ring built in %.0f ms)",
           w->center.x, w->center.z, w->chunkCount, w->cfg.chunkSize,
           (GetTime() - t0) * 1000.0);
  return w;
}

void BrushWorldUpdate(BrushWorld *w, Vector3 focus) {
  BrushChunkCoord center = ChunkOf(w, focus.x, focus.z);
  w->center = center;

  // 1) Ensure the load ring is resident (new chunks bake at their ring LOD).
  for (int dz = -w->cfg.loadRadius; dz <= w->cfg.loadRadius; dz++)
    for (int dx = -w->cfg.loadRadius; dx <= w->cfg.loadRadius; dx++) {
      BrushChunkCoord c = {center.x + dx, center.z + dz};
      if (ChunkResident(w, c)) continue;
      WorldChunk *slot = AcquireSlot(w, c);
      if (slot) {
        slot->lod = slot->desiredLod = DesiredLod(w, dx, dz, 0);
        QueueEnqueue(w->jobs, slot);
      }
    }

  // 2) Re-LOD resident chunks whose ring changed as the focus moved. ACTIVE
  //    chunks re-enter the bake queue at their new LOD; mid-build chunks get
  //    flagged and go around after finalize (adopts desiredLod there). The
  //    old mesh keeps drawing until the new one lands (no blink).
  if (w->lodEnabled) {
    for (int i = 0; i < w->chunkCount; i++) {
      WorldChunk *c = &w->chunks[i];
      ChunkState s = atomic_load(&c->state);
      if (s == CHUNK_EMPTY) continue;
      int dx = c->coord.x - center.x, dz = c->coord.z - center.z;
      int want = DesiredLod(w, dx, dz, c->lod);
      if (want == c->desiredLod && want == c->lod) continue;
      c->desiredLod = want;
      if (s == CHUNK_ACTIVE && want != c->lod) {
        c->lod = want;
        QueueEnqueue(w->jobs, c);
      } else if (want != c->lod) {
        c->needsRebake = true;
      }
    }
  }

  // 3) Finalize a bounded number of CPU-ready chunks (main-thread GPU upload).
  const int MAX_FINALIZE = 6;
  int done = 0;
  for (int i = 0; i < w->chunkCount && done < MAX_FINALIZE; i++)
    if (FinalizeChunk(w, &w->chunks[i])) done++;

  // 4) Unload outside the (larger) unload ring; hysteresis avoids border thrash.
  for (int i = 0; i < w->chunkCount; i++) {
    WorldChunk *c = &w->chunks[i];
    if (c->state != CHUNK_ACTIVE && c->state != CHUNK_CPU_READY) continue;
    int dx = c->coord.x - center.x, dz = c->coord.z - center.z;
    if (abs(dx) > w->unloadRadius || abs(dz) > w->unloadRadius)
      ReleaseChunk(w, c);
  }

  // Diagnostic: LOD distribution + triangle budget (BRUSH_LOD_DBG).
  if (getenv("BRUSH_LOD_DBG") != NULL) {
    static int frame = 0;
    if (++frame % 120 == 0) {
      int perLod[3] = {0}, active = 0;
      long gridTris = 0;
      for (int i = 0; i < w->chunkCount; i++) {
        WorldChunk *c = &w->chunks[i];
        if (atomic_load(&c->state) != CHUNK_ACTIVE || !c->meshUploaded) continue;
        active++;
        if (c->lod >= 0 && c->lod < 3) perLod[c->lod]++;
        gridTris += c->gridTriCount;
      }
      TraceLog(LOG_INFO,
               "LODDBG active=%d  L0=%d L1=%d L2=%d  gridTris=%ld (%.2fk)",
               active, perLod[0], perLod[1], perLod[2], gridTris,
               gridTris / 1000.0);
    }
  }
}

int BrushWorldGetActiveChunks(const BrushWorld *w, BrushWorldChunkView *out,
                              int max) {
  int n = 0;
  for (int i = 0; i < w->chunkCount && n < max; i++) {
    WorldChunk *c = (WorldChunk *)&w->chunks[i];
    if (atomic_load(&c->state) == CHUNK_EMPTY) continue;
    if (!c->meshUploaded || c->handle == NULL) continue;
    out[n].coord = c->coord;
    out[n].origin = WorldToRender(w, ChunkOrigin(w, c->coord));
    out[n].size = w->cfg.chunkSize;
    out[n].maxY = c->maxY;
    out[n].handle = c->handle;
    n++;
  }
  return n;
}

void BrushWorldSubmit(BrushWorld *w, Camera3D camera) {
  BrushFrustum frustum = BrushRenderMakeFrustum(camera);
  float half = w->cfg.chunkSize * 0.5f;

  // Shadow casters only matter within the far cascade's reach (~220 m by
  // default) — beyond that, terrain can't shadow anything the camera sees.
  // With LOD extending residency to ~900 m, gating this keeps the depth
  // passes from ballooning. BRUSH_SHADOW_DIST overrides (metres).
  float shadowDist = 300.0f;
  const char *sd = getenv("BRUSH_SHADOW_DIST");
  if (sd != NULL) { float v = (float)atof(sd); if (v > 0) shadowDist = v; }
  float shadowDist2 = shadowDist * shadowDist;

  for (int i = 0; i < w->chunkCount; i++) {
    WorldChunk *chunk = &w->chunks[i];
    // Draw ACTIVE chunks, and — during a sculpt rebake — chunks that are
    // back in the build pipeline but still hold their previous GPU mesh
    // (terrain must never blink out mid-stroke).
    ChunkState st = atomic_load(&chunk->state);
    if (st == CHUNK_EMPTY || !chunk->meshUploaded) continue;
    Vector3 o = ChunkOrigin(w, chunk->coord);
    Vector3 ro = WorldToRender(w, o);
    Matrix xf = MatrixTranslate(ro.x, 0.0f, ro.z);
    // CPU Frustum Culling bounds
    BoundingBox chunkBox = {
      .min = { ro.x, -50.0f, ro.z },
      .max = { ro.x + w->cfg.chunkSize, chunk->maxY + 5.0f, ro.z + w->cfg.chunkSize }
    };

    // Shadow casters aren't view-culled (terrain behind the camera still
    // casts into view when the sun is low behind you), but ARE distance-
    // gated to the shadowed range — no cascade reaches the far LOD rings.
    float scx = o.x + half - camera.position.x;
    float scz = o.z + half - camera.position.z;
    if (scx * scx + scz * scz <= shadowDist2) {
      BrushRenderSetNextBounds(chunkBox);
      BrushRenderSubmitMesh(BRUSH_LAYER_SHADOW, chunk->mesh, &w->material, xf);
    }

    if (!BrushFrustumContainsBox(&frustum, chunkBox)) continue;
    if (w->layerCount > 0 && chunk->splatTex.id != 0) {
      BrushSplatDraw sd = {0};
      sd.splat = chunk->splatTex;
      sd.origin = ro; // render-space == splat UV space (mesh verts likewise)
      sd.size = w->cfg.chunkSize;
      sd.res = w->cfg.hmRes;
      memcpy(sd.layers, w->layers, sizeof(sd.layers));
      sd.layerCount = w->layerCount;
      sd.autoSlopeLayer = w->autoSlopeLayer;
      sd.autoSlopeStart = w->autoSlopeStart;
      sd.autoSlopeEnd = w->autoSlopeEnd;
      for (int li = 0; li < BRUSH_TERRAIN_LAYERS; li++) {
        sd.layerHeightOn[li] = w->layerHeightOn[li];
        sd.layerHeightStart[li] = w->layerHeightStart[li];
        sd.layerHeightFull[li] = w->layerHeightFull[li];
      }
      if (w->hasRoadLayer && chunk->roadTex.id != 0) {
        sd.roadMask = chunk->roadTex;
        sd.roadLayer = w->roadLayer;
        sd.hasRoad = true;
      }
      BrushRenderSubmitMeshSplat(BRUSH_LAYER_OPAQUE, chunk->mesh, &w->material,
                                 xf, &sd);
    } else {
      BrushRenderSubmitMesh(BRUSH_LAYER_OPAQUE, chunk->mesh, &w->material, xf);
    }
  }
}

float BrushWorldGroundHeight(BrushWorld *w, float wx, float wz) {
  WorldChunk *c = ChunkResident(w, ChunkOf(w, wx, wz));
  if (!c || !c->heightmap) return 0.0f;
  // ACTIVE, or mid-rebake with a previous bake to read (meshUploaded). The
  // worker may be rewriting the heightmap during a rebake — a query can see
  // a transiently mixed surface for a millisecond; fine for editing flows.
  ChunkState s = atomic_load(&c->state);
  if (s != CHUNK_ACTIVE && !c->meshUploaded) return 0.0f;
  return SampleHeightmap(w, c, wx, wz);
}

void BrushWorldDestroy(BrushWorld *w) {
  if (!w) return;
  if (w->jobs) {
    if (w->jobs->started) {
      pthread_mutex_lock(&w->jobs->mutex);
      w->jobs->stop = true;
      pthread_cond_signal(&w->jobs->cond);
      pthread_mutex_unlock(&w->jobs->mutex);
      pthread_join(w->jobs->thread, NULL);
    }
    pthread_mutex_destroy(&w->jobs->mutex);
    pthread_cond_destroy(&w->jobs->cond);
    if (w->jobs->pending) MemFree(w->jobs->pending);
    MemFree(w->jobs);
  }
  for (int i = 0; i < BRUSH_WORLD_MAX_ROADS; i++)
    if (w->roadPoly[i]) MemFree(w->roadPoly[i]);
  for (int i = 0; i < w->chunkCount; i++) {
    ReleaseChunk(w, &w->chunks[i]);
    DestroyChunkBuffers(&w->chunks[i]);
  }
  if (w->chunks) MemFree(w->chunks);
  for (int i = 0; i < w->tileCount; i++) MemFree(w->tiles[i].d);
  if (w->tiles) MemFree(w->tiles);
  for (int i = 0; i < w->wtileCount; i++) MemFree(w->wtiles[i].w);
  if (w->wtiles) MemFree(w->wtiles);
  for (int i = 0; i < w->fmtileCount; i++) MemFree(w->fmtiles[i].m);
  if (w->fmtiles) MemFree(w->fmtiles);
  pthread_mutex_destroy(&w->sculptMutex);
  // The material's diffuse texture is game-owned; don't unload it. Detach the
  // shared shader so UnloadMaterial doesn't free the engine's lit shader.
  w->material.shader = (Shader){0};
  w->material.maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){0};
  UnloadMaterial(w->material);
  MemFree(w);
}

// ---- terrain sculpting (see b_world.h) --------------------------------------

// Mark every resident chunk whose SAMPLED area (incl. apron) intersects the
// world AABB for rebake. ACTIVE chunks re-enter the build queue immediately;
// chunks already mid-build get flagged and go around again after finalize.
static void SculptMarkDirty(BrushWorld *w, float minX, float minZ, float maxX,
                            float maxZ) {
  float pad = w->gridStep * 2.0f; // apron reach
  for (int i = 0; i < w->chunkCount; i++) {
    WorldChunk *c = &w->chunks[i];
    ChunkState s = atomic_load(&c->state);
    if (s == CHUNK_EMPTY) continue;
    Vector3 o = ChunkOrigin(w, c->coord);
    if (o.x - pad > maxX || o.x + w->cfg.chunkSize + pad < minX ||
        o.z - pad > maxZ || o.z + w->cfg.chunkSize + pad < minZ)
      continue;
    if (s == CHUNK_ACTIVE)
      QueueEnqueue(w->jobs, c);
    else
      c->needsRebake = true;
  }
}

static bool PassConstraints(BrushWorld *w, long gx, long gz,
                            const BrushConstraints *c) {
  if (!c) return true;
  float step = w->gridStep, wx = (float)gx * step, wz = (float)gz * step;
  if (c->checkHeight) {
    float y = Height(w, wx, wz) + SculptDeltaAt(w, gx, gz); // TOTAL Y
    if (y < c->minHeight || y > c->maxHeight) return false;
  }
  if (c->checkSlope) {
    float e = step;
    float hL = Height(w, wx - e, wz) + SculptDeltaAt(w, gx - 1, gz);
    float hR = Height(w, wx + e, wz) + SculptDeltaAt(w, gx + 1, gz);
    float hD = Height(w, wx, wz - e) + SculptDeltaAt(w, gx, gz - 1);
    float hU = Height(w, wx, wz + e) + SculptDeltaAt(w, gx, gz + 1);
    float nx = (hL - hR) / (2.0f * e), nz = (hD - hU) / (2.0f * e);
    float cosSlope = 1.0f / sqrtf(nx * nx + 1.0f + nz * nz);
    if (cosSlope < c->minCosSlope || cosSlope > c->maxCosSlope) return false;
  }
  if (c->targetLayer >= 0) {
    unsigned char wt[4];
    SplatWeightsAt(w, gx, gz, wt);
    if (wt[c->targetLayer] < 128) return false;
  }
  return true;
}

void BrushWorldSculpt(BrushWorld *w, BrushSculptOp op, Vector3 center,
                      float radius, float strength, float targetY) {
  BrushWorldSculptC(w, op, center, radius, strength, targetY, NULL);
}

void BrushWorldSculptC(BrushWorld *w, BrushSculptOp op, Vector3 center,
                       float radius, float strength, float targetY,
                       const BrushConstraints *c) {
  if (radius <= 0.0f) return;
  float step = w->gridStep;
  long g0x = (long)floorf((center.x - radius) / step);
  long g1x = (long)ceilf((center.x + radius) / step);
  long g0z = (long)floorf((center.z - radius) / step);
  long g1z = (long)ceilf((center.z + radius) / step);

  pthread_mutex_lock(&w->sculptMutex);
  for (long gz = g0z; gz <= g1z; gz++) {
    for (long gx = g0x; gx <= g1x; gx++) {
      float wx = (float)gx * step, wz = (float)gz * step;
      float dx = wx - center.x, dz = wz - center.z;
      float dist = sqrtf(dx * dx + dz * dz);
      if (dist >= radius) continue;
      if (!PassConstraints(w, gx, gz, c)) continue;
      float t = dist / radius;
      float fall = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep falloff

      float *s = SculptSampleRW(w, gx, gz);
      switch (op) {
      case BRUSH_SCULPT_ADD:
        *s += strength * fall;
        break;
      case BRUSH_SCULPT_SMOOTH: {
        float base = Height(w, wx, wz);
        float total = base + *s;
        float sum = 0.0f;
        sum += Height(w, wx - step, wz) + SculptDeltaAt(w, gx - 1, gz);
        sum += Height(w, wx + step, wz) + SculptDeltaAt(w, gx + 1, gz);
        sum += Height(w, wx, wz - step) + SculptDeltaAt(w, gx, gz - 1);
        sum += Height(w, wx, wz + step) + SculptDeltaAt(w, gx, gz + 1);
        float target = (sum + total) / 5.0f;
        float k = strength * fall;
        if (k > 1.0f) k = 1.0f;
        *s += (target - total) * k;
        break;
      }
      case BRUSH_SCULPT_FLATTEN: {
        float base = Height(w, wx, wz);
        float k = strength * fall;
        if (k > 1.0f) k = 1.0f;
        *s += ((targetY - base) - *s) * k;
        break;
      }
      }
    }
  }
  pthread_mutex_unlock(&w->sculptMutex);

  SculptMarkDirty(w, center.x - radius, center.z - radius, center.x + radius,
                  center.z + radius);
}

void BrushWorldPaint(BrushWorld *w, Vector3 center, float radius,
                     float strength, int layer) {
  BrushWorldPaintC(w, center, radius, strength, layer, NULL);
}

void BrushWorldPaintC(BrushWorld *w, Vector3 center, float radius,
                      float strength, int layer, const BrushConstraints *c) {
  if (radius <= 0.0f || layer < 0 || layer >= BRUSH_TERRAIN_LAYERS) return;
  float step = w->gridStep;
  long g0x = (long)floorf((center.x - radius) / step);
  long g1x = (long)ceilf((center.x + radius) / step);
  long g0z = (long)floorf((center.z - radius) / step);
  long g1z = (long)ceilf((center.z + radius) / step);

  pthread_mutex_lock(&w->sculptMutex);
  for (long gz = g0z; gz <= g1z; gz++) {
    for (long gx = g0x; gx <= g1x; gx++) {
      float wx = (float)gx * step, wz = (float)gz * step;
      float dx = wx - center.x, dz = wz - center.z;
      float dist = sqrtf(dx * dx + dz * dz);
      if (dist >= radius) continue;
      if (!PassConstraints(w, gx, gz, c)) continue;
      float t = dist / radius;
      float fall = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep falloff

      unsigned char *s = SplatSampleRW(w, gx, gz);
      float f[4] = {(float)s[0], (float)s[1], (float)s[2], (float)s[3]};
      f[layer] += strength * fall * 255.0f;
      float sum = f[0] + f[1] + f[2] + f[3];
      if (sum < 1.0f) { f[0] = 255.0f; sum = 255.0f; }
      for (int i = 0; i < 4; i++) {
        float v = f[i] * 255.0f / sum;
        s[i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
      }
    }
  }
  pthread_mutex_unlock(&w->sculptMutex);

  SculptMarkDirty(w, center.x - radius, center.z - radius, center.x + radius,
                  center.z + radius);
}

void BrushWorldSetLayers(BrushWorld *w,
                         const BrushTerrainLayer layers[BRUSH_TERRAIN_LAYERS],
                         int count) {
  if (count < 0) count = 0;
  if (count > BRUSH_TERRAIN_LAYERS) count = BRUSH_TERRAIN_LAYERS;
  BrushTerrainLayer next[BRUSH_TERRAIN_LAYERS] = {0};
  for (int i = 0; i < count; i++) next[i] = layers[i];
  if (count == w->layerCount &&
      memcmp(next, w->layers, sizeof(next)) == 0)
    return; // unchanged: no rebake storm
  memcpy(w->layers, next, sizeof(next));
  w->layerCount = count;
  // Every resident chunk needs its splat pixels (re)baked.
  SculptMarkDirty(w, -1e9f, -1e9f, 1e9f, 1e9f);
}

void BrushWorldRebakeAll(BrushWorld *w) {
  SculptMarkDirty(w, -1e9f, -1e9f, 1e9f, 1e9f);
}

void BrushWorldPaintFoliage(BrushWorld *w, Vector3 center, float radius,
                            float strength, int layer, bool erase) {
  if (radius <= 0.0f || layer < 0 || layer >= BRUSH_FOLIAGE_PAINT_MAX) return;
  float step = w->gridStep;
  long g0x = (long)floorf((center.x - radius) / step);
  long g1x = (long)ceilf((center.x + radius) / step);
  long g0z = (long)floorf((center.z - radius) / step);
  long g1z = (long)ceilf((center.z + radius) / step);
  float dir = erase ? -1.0f : 1.0f;

  pthread_mutex_lock(&w->sculptMutex);
  for (long gz = g0z; gz <= g1z; gz++)
    for (long gx = g0x; gx <= g1x; gx++) {
      float wx = (float)gx * step, wz = (float)gz * step;
      float dx = wx - center.x, dz = wz - center.z;
      float dist = sqrtf(dx * dx + dz * dz);
      if (dist >= radius) continue;
      float t = dist / radius;
      float fall = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep
      unsigned char *m = FoliageMaskRW(w, gx, gz);
      float v = (float)m[layer] + dir * strength * fall * 255.0f;
      m[layer] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
    }
  pthread_mutex_unlock(&w->sculptMutex);

  // Only the painted chunks re-bake (mask re-composes) + re-scatter.
  SculptMarkDirty(w, center.x - radius, center.z - radius, center.x + radius,
                  center.z + radius);
}

void BrushWorldSetAutoSlope(BrushWorld *w, int layer, float startDeg,
                            float endDeg) {
  w->autoSlopeLayer = (layer >= 0 && layer < BRUSH_TERRAIN_LAYERS) ? layer : -1;
  if (endDeg < startDeg + 1.0f) endDeg = startDeg + 1.0f;
  w->autoSlopeStart = startDeg;
  w->autoSlopeEnd = endDeg;
}

void BrushWorldSetLayerHeights(BrushWorld *w, const int *on,
                               const float *start, const float *full) {
  for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++) {
    w->layerHeightOn[i] = (on && on[i]) ? 1 : 0;
    w->layerHeightStart[i] = start ? start[i] : 0.0f;
    w->layerHeightFull[i] = full ? full[i] : 0.0f;
  }
}

int BrushWorldSurfaceAt(BrushWorld *w, float wx, float wz) {
  if (w->layerCount <= 0) return -1;
  long gx = (long)floorf(wx / w->gridStep + 0.5f);
  long gz = (long)floorf(wz / w->gridStep + 0.5f);
  unsigned char wt[4];
  pthread_mutex_lock(&w->sculptMutex);
  SplatWeightsAt(w, gx, gz, wt);
  pthread_mutex_unlock(&w->sculptMutex);
  int best = 0;
  for (int i = 1; i < w->layerCount; i++)
    if (wt[i] > wt[best]) best = i;
  return best;
}

bool BrushWorldSculptAny(const BrushWorld *w) {
  return w->tileCount > 0 || w->wtileCount > 0 || w->fmtileCount > 0;
}

// Blob layout (also the .terrain file format). BSC2 appends the paint weights
// after the height tiles; BSC3 appends the foliage density mask after that.
// Older files (BSC1 heights-only, BSC2 heights+weights) still load.
//   u32 magic 'BSC3' | i32 tileRes | i32 count
//   i32 rangeT0x rangeT0z rangeT1x rangeT1z   (tile range the blob covers)
//   count  x { i32 tx, i32 tz, float[tileRes^2] }          (height deltas)
//   i32 wcount
//   wcount x { i32 tx, i32 tz, u8[tileRes^2 * 4] }          (layer weights)
//   i32 fmcount                                             (BSC3+)
//   fmcount x { i32 tx, i32 tz, u8[tileRes^2 * 4] }         (foliage density)
#define SCULPT_MAGIC 0x31435342u  // "BSC1"
#define SCULPT_MAGIC2 0x32435342u // "BSC2"
#define SCULPT_MAGIC3 0x33435342u // "BSC3"

static unsigned char *SculptSerialize(BrushWorld *w, int t0x, int t0z,
                                      int t1x, int t1z, int *outSize) {
  int n = w->tileRes * w->tileRes;
  int count = 0, wcount = 0, fmcount = 0;
  for (int i = 0; i < w->tileCount; i++) {
    SculptTile *t = &w->tiles[i];
    if (t->tx >= t0x && t->tx <= t1x && t->tz >= t0z && t->tz <= t1z) count++;
  }
  for (int i = 0; i < w->wtileCount; i++) {
    SplatTile *t = &w->wtiles[i];
    if (t->tx >= t0x && t->tx <= t1x && t->tz >= t0z && t->tz <= t1z) wcount++;
  }
  for (int i = 0; i < w->fmtileCount; i++) {
    FoliageMaskTile *t = &w->fmtiles[i];
    if (t->tx >= t0x && t->tx <= t1x && t->tz >= t0z && t->tz <= t1z) fmcount++;
  }
  int size = (int)(sizeof(unsigned int) + 7 * sizeof(int) +
                   (size_t)count * (2 * sizeof(int) + n * sizeof(float)) +
                   (size_t)wcount * (2 * sizeof(int) + (size_t)n * 4) +
                   sizeof(int) + // fmcount
                   (size_t)fmcount * (2 * sizeof(int) + (size_t)n * 4));
  unsigned char *blob = (unsigned char *)MemAlloc((unsigned int)size);
  unsigned char *p = blob;
  *(unsigned int *)p = SCULPT_MAGIC3; p += 4;
  *(int *)p = w->tileRes; p += 4;
  *(int *)p = count; p += 4;
  *(int *)p = t0x; p += 4;
  *(int *)p = t0z; p += 4;
  *(int *)p = t1x; p += 4;
  *(int *)p = t1z; p += 4;
  for (int i = 0; i < w->tileCount; i++) {
    SculptTile *t = &w->tiles[i];
    if (t->tx < t0x || t->tx > t1x || t->tz < t0z || t->tz > t1z) continue;
    *(int *)p = t->tx; p += 4;
    *(int *)p = t->tz; p += 4;
    memcpy(p, t->d, sizeof(float) * (size_t)n); p += sizeof(float) * (size_t)n;
  }
  *(int *)p = wcount; p += 4;
  for (int i = 0; i < w->wtileCount; i++) {
    SplatTile *t = &w->wtiles[i];
    if (t->tx < t0x || t->tx > t1x || t->tz < t0z || t->tz > t1z) continue;
    *(int *)p = t->tx; p += 4;
    *(int *)p = t->tz; p += 4;
    memcpy(p, t->w, (size_t)n * 4); p += (size_t)n * 4;
  }
  *(int *)p = fmcount; p += 4;
  for (int i = 0; i < w->fmtileCount; i++) {
    FoliageMaskTile *t = &w->fmtiles[i];
    if (t->tx < t0x || t->tx > t1x || t->tz < t0z || t->tz > t1z) continue;
    *(int *)p = t->tx; p += 4;
    *(int *)p = t->tz; p += 4;
    memcpy(p, t->m, (size_t)n * 4); p += (size_t)n * 4;
  }
  if (outSize) *outSize = size;
  return blob;
}

unsigned char *BrushWorldSculptSnapshot(BrushWorld *w, Vector2 minXZ,
                                        Vector2 maxXZ, int *outSize) {
  float tileWorld = w->gridStep * (float)w->tileRes;
  int t0x = (int)floorf(minXZ.x / tileWorld), t1x = (int)floorf(maxXZ.x / tileWorld);
  int t0z = (int)floorf(minXZ.y / tileWorld), t1z = (int)floorf(maxXZ.y / tileWorld);
  pthread_mutex_lock(&w->sculptMutex);
  unsigned char *blob = SculptSerialize(w, t0x, t0z, t1x, t1z, outSize);
  pthread_mutex_unlock(&w->sculptMutex);
  return blob;
}

void BrushWorldSculptRestore(BrushWorld *w, const unsigned char *blob,
                             int size) {
  if (blob == NULL || size < 28) return;
  const unsigned char *p = blob;
  unsigned int magic = *(const unsigned int *)p;
  if (magic != SCULPT_MAGIC && magic != SCULPT_MAGIC2 && magic != SCULPT_MAGIC3)
    return;
  bool hasWeights = (magic == SCULPT_MAGIC2 || magic == SCULPT_MAGIC3);
  bool hasMask = (magic == SCULPT_MAGIC3);
  p += 4;
  int tileRes = *(const int *)p; p += 4;
  int count = *(const int *)p; p += 4;
  int t0x = *(const int *)p; p += 4;
  int t0z = *(const int *)p; p += 4;
  int t1x = *(const int *)p; p += 4;
  int t1z = *(const int *)p; p += 4;
  if (tileRes != w->tileRes) {
    TraceLog(LOG_WARNING, "BRUSH sculpt: blob tileRes %d != world %d", tileRes,
             w->tileRes);
    return;
  }
  int n = w->tileRes * w->tileRes;

  pthread_mutex_lock(&w->sculptMutex);
  // Tiles created after the snapshot (inside its range) must return to zero
  // (weights: to the full-layer-0 default).
  for (int i = 0; i < w->tileCount; i++) {
    SculptTile *t = &w->tiles[i];
    if (t->tx >= t0x && t->tx <= t1x && t->tz >= t0z && t->tz <= t1z)
      memset(t->d, 0, sizeof(float) * (size_t)n);
  }
  for (int i = 0; i < count; i++) {
    int tx = *(const int *)p; p += 4;
    int tz = *(const int *)p; p += 4;
    SculptTile *t = SculptGetTile(w, tx, tz, true);
    memcpy(t->d, p, sizeof(float) * (size_t)n);
    p += sizeof(float) * (size_t)n;
  }
  if (hasWeights) {
    for (int i = 0; i < w->wtileCount; i++) {
      SplatTile *t = &w->wtiles[i];
      if (t->tx >= t0x && t->tx <= t1x && t->tz >= t0z && t->tz <= t1z)
        SplatTileDefault(w, t);
    }
    int wcount = *(const int *)p; p += 4;
    for (int i = 0; i < wcount; i++) {
      int tx = *(const int *)p; p += 4;
      int tz = *(const int *)p; p += 4;
      SplatTile *t = SplatGetTile(w, tx, tz, true);
      memcpy(t->w, p, (size_t)n * 4);
      p += (size_t)n * 4;
    }
  }
  if (hasMask) {
    // In-range mask tiles created after the snapshot return to the neutral
    // default (M = 1x base).
    for (int i = 0; i < w->fmtileCount; i++) {
      FoliageMaskTile *t = &w->fmtiles[i];
      if (t->tx >= t0x && t->tx <= t1x && t->tz >= t0z && t->tz <= t1z)
        memset(t->m, FOLIAGE_MASK_NEUTRAL, (size_t)n * 4);
    }
    int fmcount = *(const int *)p; p += 4;
    for (int i = 0; i < fmcount; i++) {
      int tx = *(const int *)p; p += 4;
      int tz = *(const int *)p; p += 4;
      FoliageMaskTile *t = FoliageMaskGet(w, tx, tz, true);
      memcpy(t->m, p, (size_t)n * 4);
      p += (size_t)n * 4;
    }
  }
  pthread_mutex_unlock(&w->sculptMutex);

  float tileWorld = w->gridStep * (float)w->tileRes;
  SculptMarkDirty(w, (float)t0x * tileWorld, (float)t0z * tileWorld,
                  (float)(t1x + 1) * tileWorld, (float)(t1z + 1) * tileWorld);
}

bool BrushWorldSculptSave(BrushWorld *w, const char *path) {
  pthread_mutex_lock(&w->sculptMutex);
  int t0x = 0, t0z = 0, t1x = -1, t1z = -1; // empty range when no tiles
  bool first = true;
  for (int i = 0; i < w->tileCount; i++) { // range spans BOTH overlays
    SculptTile *t = &w->tiles[i];
    if (first) { t0x = t1x = t->tx; t0z = t1z = t->tz; first = false; }
    if (t->tx < t0x) t0x = t->tx;
    if (t->tx > t1x) t1x = t->tx;
    if (t->tz < t0z) t0z = t->tz;
    if (t->tz > t1z) t1z = t->tz;
  }
  for (int i = 0; i < w->wtileCount; i++) {
    SplatTile *t = &w->wtiles[i];
    if (first) { t0x = t1x = t->tx; t0z = t1z = t->tz; first = false; }
    if (t->tx < t0x) t0x = t->tx;
    if (t->tx > t1x) t1x = t->tx;
    if (t->tz < t0z) t0z = t->tz;
    if (t->tz > t1z) t1z = t->tz;
  }
  for (int i = 0; i < w->fmtileCount; i++) {
    FoliageMaskTile *t = &w->fmtiles[i];
    if (first) { t0x = t1x = t->tx; t0z = t1z = t->tz; first = false; }
    if (t->tx < t0x) t0x = t->tx;
    if (t->tx > t1x) t1x = t->tx;
    if (t->tz < t0z) t0z = t->tz;
    if (t->tz > t1z) t1z = t->tz;
  }
  int size = 0;
  unsigned char *blob = SculptSerialize(w, t0x, t0z, t1x, t1z, &size);
  pthread_mutex_unlock(&w->sculptMutex);

  bool ok = SaveFileData(path, blob, size);
  MemFree(blob);
  if (ok)
    TraceLog(LOG_INFO, "BRUSH sculpt: saved %s (%d tiles)", path,
             w->tileCount);
  return ok;
}

bool BrushWorldSculptLoad(BrushWorld *w, const char *path) {
  int size = 0;
  unsigned char *blob = LoadFileData(path, &size);
  if (blob == NULL) return false;
  BrushWorldSculptRestore(w, blob, size);
  UnloadFileData(blob);
  TraceLog(LOG_INFO, "BRUSH sculpt: loaded %s (%d tiles)", path, w->tileCount);
  return true;
}

static Vector3 CatmullRom(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t) {
  float t2 = t * t;
  float t3 = t2 * t;

  float f0 = -0.5f * t3 + t2 - 0.5f * t;
  float f1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
  float f2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
  float f3 = 0.5f * t3 - 0.5f * t2;

  Vector3 r;
  r.x = p0.x * f0 + p1.x * f1 + p2.x * f2 + p3.x * f3;
  r.y = p0.y * f0 + p1.y * f1 + p2.y * f2 + p3.y * f3;
  r.z = p0.z * f0 + p1.z * f1 + p2.z * f2 + p3.z * f3;
  return r;
}

static float RoadNearest(const Vector3 *poly, int n, float wx, float wz,
                         float *outRoadY) {
  float best = 1e30f, bestY = 0.0f;
  for (int i = 0; i + 1 < n; i++) {
    Vector2 A = {poly[i].x,   poly[i].z};
    Vector2 B = {poly[i+1].x, poly[i+1].z};
    Vector2 AB = {B.x - A.x, B.y - A.y};
    float len2 = AB.x * AB.x + AB.y * AB.y;
    float u = len2 > 1e-6f ? ((wx - A.x) * AB.x + (wz - A.y) * AB.y) / len2 : 0.0f;
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    float cx = A.x + AB.x * u, cz = A.y + AB.y * u;
    float d = sqrtf((wx - cx) * (wx - cx) + (wz - cz) * (wz - cz));
    if (d < best) {
      best = d;
      bestY = poly[i].y + (poly[i+1].y - poly[i].y) * u;
    }
  }
  *outRoadY = bestY;
  return best;
}

Vector3 *BrushWorldRoadPolyline(const BrushWorld *w, const Vector3 *points,
                                int count, int *outN) {
  if (points == NULL || count < 2) {
    if (outN) *outN = 0;
    return NULL;
  }
  // Size first (same per-segment sample count the fill loop uses), then fill:
  // one polyline shared by the stamp and the editor preview.
  int total = 0;
  for (int i = 0; i < count - 1; i++) {
    Vector3 a = points[i], b = points[i + 1];
    float segLen = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) +
                         (b.z - a.z) * (b.z - a.z));
    int samples = (int)ceilf(segLen / w->gridStep);
    if (samples < 4) samples = 4;
    total += samples;
  }
  total += 1; // closing point

  Vector3 *poly = (Vector3 *)MemAlloc(sizeof(Vector3) * total);
  int idx = 0;
  for (int i = 0; i < count - 1; i++) {
    Vector3 p0 = (i == 0) ? points[0] : points[i - 1];
    Vector3 p1 = points[i];
    Vector3 p2 = points[i + 1];
    Vector3 p3 = (i + 2 >= count) ? points[count - 1] : points[i + 2];
    float segLen = sqrtf((p2.x - p1.x) * (p2.x - p1.x) +
                         (p2.y - p1.y) * (p2.y - p1.y) +
                         (p2.z - p1.z) * (p2.z - p1.z));
    int samples = (int)ceilf(segLen / w->gridStep);
    if (samples < 4) samples = 4;
    for (int s = 0; s < samples; s++)
      poly[idx++] = CatmullRom(p0, p1, p2, p3, (float)s / (float)samples);
  }
  poly[idx++] = points[count - 1];
  if (outN) *outN = idx;
  return poly;
}

// Distance (XZ) from world (wx,wz) to road i, with its surface Y at the nearest
// point. Returns -1 when outside the road's WIDEST influence (AABB + max edge);
// the caller applies its own smoothstep with the height or texture margin.
static float RoadDistance(const BrushWorld *w, int i, float wx, float wz,
                          float *y) {
  if (w->roadPolyN[i] < 2) return -1.0f;
  if (wx < w->roadAABB[i][0] || wx > w->roadAABB[i][2] ||
      wz < w->roadAABB[i][1] || wz > w->roadAABB[i][3])
    return -1.0f;
  return RoadNearest(w->roadPoly[i], w->roadPolyN[i], wx, wz, y);
}

// Corridor mask: 1 inside `half`, smoothstep to 0 across `fade`; fade 0 = hard
// edge (paving stops crisply at the corridor width).
static float RoadMask(float D, float half, float fade) {
  if (D <= half) return 1.0f;
  if (fade < 1e-4f || D >= half + fade) return 0.0f;
  float t = (D - half) / fade;
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

// Bake hook: flatten the surface height toward every overlapping road (the
// GEOMETRY shoulder always uses `fade`, so the ground eases even for a
// hard-edged paved texture — no cliff at the road edge).
static float RoadHeightAt(const BrushWorld *w, float wx, float wz, float h) {
  for (int i = 0; i < w->roadCount; i++) {
    float roadY, D = RoadDistance(w, i, wx, wz, &roadY);
    if (D < 0.0f) continue;
    float m = RoadMask(D, w->roads[i].width * 0.5f, w->roads[i].fade);
    if (m > 0.0f) h += (roadY - h) * m;
  }
  return h;
}

// Bake hook: drive the road layer weight dominant across every overlapping
// road, using the TEXTURE margin `paintFade` (0 = hard paved edge). Sum-255
// preserving so it doesn't fight the renormalise.
// Road surface coverage at a world point: the strongest road mask over all
// roads (0..255). Roads with layerSlot < 0 still contribute — the slot field is
// now only a legacy carve hint; texturing is the dedicated road material.
static unsigned char RoadCoverageAt(const BrushWorld *w, float wx, float wz) {
  float cov = 0.0f;
  for (int i = 0; i < w->roadCount; i++) {
    float roadY, D = RoadDistance(w, i, wx, wz, &roadY);
    if (D < 0.0f) continue;
    float m = RoadMask(D, w->roads[i].width * 0.5f, w->roads[i].paintFade);
    if (m > cov) cov = m;
  }
  return (unsigned char)(cov <= 0.0f ? 0 : (cov >= 1.0f ? 255 : cov * 255.0f + 0.5f));
}

// (Re)build road i's cached polyline + world AABB (edge-expanded). Caller holds
// sculptMutex. Frees any previous polyline first.
static void RoadCacheBuild(BrushWorld *w, int i) {
  if (w->roadPoly[i]) {
    MemFree(w->roadPoly[i]);
    w->roadPoly[i] = NULL;
  }
  w->roadPolyN[i] = 0;
  const BrushWorldRoad *r = &w->roads[i];
  if (r->pointCount < 2) return;
  int n = 0;
  w->roadPoly[i] = BrushWorldRoadPolyline(w, r->points, r->pointCount, &n);
  w->roadPolyN[i] = n;
  float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
  for (int k = 0; k < n; k++) {
    Vector3 p = w->roadPoly[i][k];
    if (p.x < minX) minX = p.x;
    if (p.x > maxX) maxX = p.x;
    if (p.z < minZ) minZ = p.z;
    if (p.z > maxZ) maxZ = p.z;
  }
  float edge = r->width * 0.5f + fmaxf(r->fade, r->paintFade);
  w->roadAABB[i][0] = minX - edge;
  w->roadAABB[i][1] = minZ - edge;
  w->roadAABB[i][2] = maxX + edge;
  w->roadAABB[i][3] = maxZ + edge;
}

void BrushWorldSetRoads(BrushWorld *w, const BrushWorldRoad *roads, int count) {
  if (count < 0) count = 0;
  if (count > BRUSH_WORLD_MAX_ROADS) count = BRUSH_WORLD_MAX_ROADS;

  // Chunks overlapping OLD roads must rebake too (so a shrunk/moved/removed
  // road restores the terrain it used to cover). Collect old + new AABBs, then
  // mark dirty OUTSIDE the lock (SculptMarkDirty touches the job queue).
  float dirty[BRUSH_WORLD_MAX_ROADS * 2][4];
  int dirtyN = 0;

  pthread_mutex_lock(&w->sculptMutex);
  for (int i = 0; i < w->roadCount; i++)
    if (w->roadPolyN[i] > 0 && dirtyN < BRUSH_WORLD_MAX_ROADS * 2)
      memcpy(dirty[dirtyN++], w->roadAABB[i], sizeof(float) * 4);

  for (int i = 0; i < BRUSH_WORLD_MAX_ROADS; i++) {
    if (w->roadPoly[i]) {
      MemFree(w->roadPoly[i]);
      w->roadPoly[i] = NULL;
      w->roadPolyN[i] = 0;
    }
  }
  w->roadCount = count;
  for (int i = 0; i < count; i++) {
    w->roads[i] = roads[i];
    RoadCacheBuild(w, i);
    if (w->roadPolyN[i] > 0 && dirtyN < BRUSH_WORLD_MAX_ROADS * 2)
      memcpy(dirty[dirtyN++], w->roadAABB[i], sizeof(float) * 4);
  }
  pthread_mutex_unlock(&w->sculptMutex);

  for (int i = 0; i < dirtyN; i++)
    SculptMarkDirty(w, dirty[i][0], dirty[i][1], dirty[i][2], dirty[i][3]);
}

void BrushWorldSetRoadMaterial(BrushWorld *w, const BrushTerrainLayer *mat) {
  float dirty[BRUSH_WORLD_MAX_ROADS][4];
  int dirtyN = 0;
  pthread_mutex_lock(&w->sculptMutex);
  if (mat != NULL && mat->albedo.id != 0) {
    w->roadLayer = *mat;
    w->hasRoadLayer = true;
  } else {
    w->roadLayer = (BrushTerrainLayer){0};
    w->hasRoadLayer = false;
  }
  for (int i = 0; i < w->roadCount; i++)
    if (w->roadPolyN[i] > 0 && dirtyN < BRUSH_WORLD_MAX_ROADS)
      memcpy(dirty[dirtyN++], w->roadAABB[i], sizeof(float) * 4);
  pthread_mutex_unlock(&w->sculptMutex);
  for (int i = 0; i < dirtyN; i++) // rebake road-overlapping chunks with the mask
    SculptMarkDirty(w, dirty[i][0], dirty[i][1], dirty[i][2], dirty[i][3]);
}

float BrushWorldGetGridStep(const BrushWorld *w) {
  return w->gridStep;
}
