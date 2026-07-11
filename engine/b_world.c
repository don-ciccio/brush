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
} WorldChunk;

struct BrushChunkJobQueue; // fwd

struct BrushWorld {
  BrushWorldConfig cfg;
  BrushChunkCoord center;
  BrushChunkCoord origin; // rebase seam origin (identity today)

  int hmTexRes;  // hmRes + 2 (apron)
  int unloadRadius;
  int maxChunks;

  WorldChunk *chunks;
  int chunkCount;

  Material material; // shared across every chunk mesh
  bool ownsGroundTex;

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

// Build the displaced terrain mesh CPU-side. Vertices are LOCAL [0,size] with
// Y = Height(world); normals from central differences of the same field. Edge
// vertices land on exact shared world coords, so surface + normals are seamless
// across borders. Texcoords tile in WORLD space.
static void BuildMesh(const BrushWorld *w, WorldChunk *chunk) {
  const int R = w->cfg.meshRes;
  const float cell = w->cfg.chunkSize / (float)(R - 1);
  const float e = 1.0f; // normal epsilon (m)
  const float texScale = 1.0f / w->cfg.texMetresPerTile;
  Vector3 o = ChunkOrigin(w, chunk->coord);

  Mesh m = chunk->mesh;
  if (m.vertices == NULL) {
    m.vertexCount = R * R;
    m.triangleCount = (R - 1) * (R - 1) * 2;
    m.vertices = (float *)MemAlloc(sizeof(float) * m.vertexCount * 3);
    m.normals = (float *)MemAlloc(sizeof(float) * m.vertexCount * 3);
    m.texcoords = (float *)MemAlloc(sizeof(float) * m.vertexCount * 2);
    m.colors = (unsigned char *)MemAlloc(sizeof(unsigned char) * m.vertexCount * 4);
    m.indices =
        (unsigned short *)MemAlloc(sizeof(unsigned short) * m.triangleCount * 3);
    int t = 0;
    for (int j = 0; j < R - 1; j++) {
      for (int i = 0; i < R - 1; i++) {
        unsigned short a = (unsigned short)(j * R + i);
        unsigned short b = (unsigned short)(j * R + i + 1);
        unsigned short c = (unsigned short)((j + 1) * R + i);
        unsigned short d = (unsigned short)((j + 1) * R + i + 1);
        m.indices[t++] = a; m.indices[t++] = c; m.indices[t++] = b;
        m.indices[t++] = b; m.indices[t++] = c; m.indices[t++] = d;
      }
    }
    chunk->mesh = m;
  }

  float maxH = -1e30f;
  for (int j = 0; j < R; j++) {
    for (int i = 0; i < R; i++) {
      float lx = (float)i * cell, lz = (float)j * cell;
      float wx = o.x + lx, wz = o.z + lz;
      float h = Height(w, wx, wz);
      float hL = Height(w, wx - e, wz), hR = Height(w, wx + e, wz);
      float hD = Height(w, wx, wz - e), hU = Height(w, wx, wz + e);
      Vector3 n = Vector3Normalize((Vector3){hL - hR, 2.0f * e, hD - hU});
      int v = j * R + i;
      chunk->mesh.vertices[v * 3 + 0] = lx;
      chunk->mesh.vertices[v * 3 + 1] = h;
      chunk->mesh.vertices[v * 3 + 2] = lz;
      chunk->mesh.normals[v * 3 + 0] = n.x;
      chunk->mesh.normals[v * 3 + 1] = n.y;
      chunk->mesh.normals[v * 3 + 2] = n.z;
      chunk->mesh.texcoords[v * 2 + 0] = wx * texScale;
      chunk->mesh.texcoords[v * 2 + 1] = wz * texScale;
      // With a ground texture, keep vertex colours neutral so the texture
      // reads true; without one, shade by slope (green lowland / grey rock).
      if (w->cfg.groundTex.id > 0) {
        unsigned char *col = &chunk->mesh.colors[v * 4];
        col[0] = col[1] = col[2] = col[3] = 255;
      } else {
        SlopeColor(n.y, &chunk->mesh.colors[v * 4]);
      }
      if (h > maxH) maxH = h;
    }
  }
  chunk->maxY = maxH;
}

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
  BuildMesh(w, chunk);

  // Cook the Jolt collider here on the worker: the BVH build is the
  // expensive half of collider creation and needs no physics-world access.
  // The main-thread finalize just wraps a body around it.
  if (w->cfg.physics) {
    Vector3 ro = WorldToRender(w, o);
    Matrix xf = MatrixTranslate(ro.x, 0.0f, ro.z);
    chunk->pendingShape = BrushPhysicsCookMeshShape(chunk->mesh, xf);
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
      memset(k, 0, sizeof(*k));
      k->coord = c;
      k->heightmap = hm;
      k->mesh = mesh;
      k->meshUploaded = up;
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
    if (!chunk->meshUploaded) {
      UploadMesh(&chunk->mesh, false);
      chunk->meshUploaded = true;
    } else {
      // Recycle the GPU buffers in place.
      int vc = chunk->mesh.vertexCount;
      UpdateMeshBuffer(chunk->mesh, 0, chunk->mesh.vertices, sizeof(float) * vc * 3, 0);
      UpdateMeshBuffer(chunk->mesh, 1, chunk->mesh.texcoords, sizeof(float) * vc * 2, 0);
      UpdateMeshBuffer(chunk->mesh, 2, chunk->mesh.normals, sizeof(float) * vc * 3, 0);
      UpdateMeshBuffer(chunk->mesh, 3, chunk->mesh.colors, sizeof(unsigned char) * vc * 4, 0);
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

  atomic_store(&chunk->state, CHUNK_ACTIVE);
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
  // Keep heightmap + mesh buffers allocated for slot reuse.
  chunk->state = CHUNK_EMPTY;
}

static void DestroyChunkBuffers(WorldChunk *chunk) {
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

  BrushWorld *w = (BrushWorld *)MemAlloc(sizeof(BrushWorld));
  memset(w, 0, sizeof(*w));
  w->cfg = cfg;
  w->hmTexRes = cfg.hmRes + 2;
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
           "BRUSH world: ready at chunk (%d,%d), %d chunks, %.0fm each",
           w->center.x, w->center.z, w->chunkCount, w->cfg.chunkSize);
  return w;
}

void BrushWorldUpdate(BrushWorld *w, Vector3 focus) {
  BrushChunkCoord center = ChunkOf(w, focus.x, focus.z);
  w->center = center;

  // 1) Ensure the load ring is resident.
  for (int dz = -w->cfg.loadRadius; dz <= w->cfg.loadRadius; dz++)
    for (int dx = -w->cfg.loadRadius; dx <= w->cfg.loadRadius; dx++) {
      BrushChunkCoord c = {center.x + dx, center.z + dz};
      if (ChunkResident(w, c)) continue;
      WorldChunk *slot = AcquireSlot(w, c);
      if (slot) QueueEnqueue(w->jobs, slot);
    }

  // 2) Finalize a bounded number of CPU-ready chunks (main-thread GPU upload).
  const int MAX_FINALIZE = 6;
  int done = 0;
  for (int i = 0; i < w->chunkCount && done < MAX_FINALIZE; i++)
    if (FinalizeChunk(w, &w->chunks[i])) done++;

  // 3) Unload outside the (larger) unload ring; hysteresis avoids border thrash.
  for (int i = 0; i < w->chunkCount; i++) {
    WorldChunk *c = &w->chunks[i];
    if (c->state != CHUNK_ACTIVE && c->state != CHUNK_CPU_READY) continue;
    int dx = c->coord.x - center.x, dz = c->coord.z - center.z;
    if (abs(dx) > w->unloadRadius || abs(dz) > w->unloadRadius)
      ReleaseChunk(w, c);
  }
}

void BrushWorldSubmit(BrushWorld *w, Camera3D camera) {
  // Camera-forward (XZ) for a cheap behind-the-view cull.
  float fx = camera.target.x - camera.position.x;
  float fz = camera.target.z - camera.position.z;
  float fl = sqrtf(fx * fx + fz * fz);
  if (fl > 1e-4f) { fx /= fl; fz /= fl; } else { fx = 0; fz = 1; }
  float half = w->cfg.chunkSize * 0.5f;

  for (int i = 0; i < w->chunkCount; i++) {
    WorldChunk *chunk = &w->chunks[i];
    if (atomic_load(&chunk->state) != CHUNK_ACTIVE || !chunk->meshUploaded)
      continue;
    Vector3 o = ChunkOrigin(w, chunk->coord);
    Vector3 ro = WorldToRender(w, o);
    Matrix xf = MatrixTranslate(ro.x, 0.0f, ro.z);
    // Shadows are submitted UNCONDITIONALLY: the camera cull below is about
    // the VIEW — terrain behind the camera still casts shadows into it when
    // the sun is low behind you. The depth pass is cheap; missing casters pop.
    BrushRenderSubmitMesh(BRUSH_LAYER_SHADOW, chunk->mesh, &w->material, xf);

    float ccx = o.x + half - camera.position.x;
    float ccz = o.z + half - camera.position.z;
    float cd = sqrtf(ccx * ccx + ccz * ccz);
    if (cd > w->cfg.chunkSize * 1.6f) {
      float dot = (ccx * fx + ccz * fz) / cd;
      if (dot < -0.15f) continue; // clearly behind the camera
    }
    BrushRenderSubmitMesh(BRUSH_LAYER_OPAQUE, chunk->mesh, &w->material, xf);
  }
}

float BrushWorldGroundHeight(BrushWorld *w, float wx, float wz) {
  WorldChunk *c = ChunkResident(w, ChunkOf(w, wx, wz));
  if (!c || atomic_load(&c->state) != CHUNK_ACTIVE || !c->heightmap)
    return 0.0f;
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
  for (int i = 0; i < w->chunkCount; i++) {
    ReleaseChunk(w, &w->chunks[i]);
    DestroyChunkBuffers(&w->chunks[i]);
  }
  if (w->chunks) MemFree(w->chunks);
  // The material's diffuse texture is game-owned; don't unload it. Detach the
  // shared shader so UnloadMaterial doesn't free the engine's lit shader.
  w->material.shader = (Shader){0};
  w->material.maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){0};
  UnloadMaterial(w->material);
  MemFree(w);
}
