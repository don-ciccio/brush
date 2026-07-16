/*******************************************************************************************
 *   b_foliage.c - CPU-side scatter / spatial-grid cull / LOD machinery for
 *                 instanced foliage. Ported from the donor game's foliage.c;
 *                 see docs/foliage-plan.md (roadmap v1 #7, Phase 2).
 *
 *   Refactored from the donor in two ways to match the brush thread-safety
 *   design (foliage-plan.md §4):
 *     - The per-set visible/scratch buffers are GONE. A set is static data
 *       only; the cull takes it `const` and appends survivors into caller-owned
 *       per-band buffers, so many chunks accumulate into one draw per band.
 *     - Decimation goes through BrushMeshDecimate (b_meshlod).
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_foliage.h"
#include "b_assets.h"  // BrushEnginePath
#include "b_meshlod.h"
#include "b_render.h"  // BrushRenderSetSceneCallback / ApplySceneLighting

#include <raymath.h>
#include <rlgl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void BrushFoliageScatterGrid(BrushFoliageSet *set, Vector3 center, float width,
                             float depth, float density, float maxMultiplier,
                             int maxCountCap, BrushFoliagePlaceFn placeFn,
                             void *userData) {
  float area = width * depth;
  int maxCount = (int)(area * density * maxMultiplier);
  if (maxCount > maxCountCap) maxCount = maxCountCap;
  if (maxCount < 1) maxCount = 1;

  set->transforms = (Matrix *)MemAlloc(sizeof(Matrix) * maxCount);
  set->positions = (Vector3 *)MemAlloc(sizeof(Vector3) * maxCount);
  set->modelIdx = (unsigned char *)MemAlloc(sizeof(unsigned char) * maxCount);

  float halfW = width / 2.0f;
  float halfD = depth / 2.0f;
  int gridResX = (int)(width * sqrtf(density));
  int gridResZ = (int)(depth * sqrtf(density));
  if (gridResX < 1) gridResX = 1;
  if (gridResZ < 1) gridResZ = 1;
  float cellW = width / (float)gridResX;
  float cellD = depth / (float)gridResZ;

  int index = 0;
  for (int gx = 0; gx < gridResX && index < maxCount; gx++) {
    for (int gz = 0; gz < gridResZ && index < maxCount; gz++) {
      float baseX = center.x - halfW + (gx + 0.5f) * cellW;
      float baseZ = center.z - halfD + (gz + 0.5f) * cellD;
      placeFn(userData, set, &index, maxCount, gx, gz, baseX, baseZ, cellW,
              cellD, center.y);
    }
  }

  set->count = index;

  // Shrink the allocation to the actual instance count.
  if (index < maxCount && index > 0) {
    Matrix *trimmedT = (Matrix *)MemAlloc(sizeof(Matrix) * index);
    memcpy(trimmedT, set->transforms, sizeof(Matrix) * index);
    MemFree(set->transforms);
    set->transforms = trimmedT;

    Vector3 *trimmedP = (Vector3 *)MemAlloc(sizeof(Vector3) * index);
    memcpy(trimmedP, set->positions, sizeof(Vector3) * index);
    MemFree(set->positions);
    set->positions = trimmedP;

    unsigned char *trimmedM = (unsigned char *)MemAlloc(sizeof(unsigned char) * index);
    memcpy(trimmedM, set->modelIdx, sizeof(unsigned char) * index);
    MemFree(set->modelIdx);
    set->modelIdx = trimmedM;
  }
}

static void WorldToCell(const BrushFoliageSet *set, float worldX, float worldZ,
                        int *cx, int *cz) {
  *cx = (int)floorf((worldX - set->originX) / set->cellSize);
  *cz = (int)floorf((worldZ - set->originZ) / set->cellSize);
  if (*cx < 0) *cx = 0;
  if (*cx >= set->gridResX) *cx = set->gridResX - 1;
  if (*cz < 0) *cz = 0;
  if (*cz >= set->gridResZ) *cz = set->gridResZ - 1;
}

void BrushFoliageBuildGrid(BrushFoliageSet *set, Vector3 center, float width,
                           float depth, float cellSize) {
  set->cellSize = (cellSize > 0.001f) ? cellSize : 1.0f;
  set->originX = center.x - width / 2.0f;
  set->originZ = center.z - depth / 2.0f;
  set->gridResX = (int)ceilf(width / set->cellSize);
  set->gridResZ = (int)ceilf(depth / set->cellSize);
  if (set->gridResX < 1) set->gridResX = 1;
  if (set->gridResZ < 1) set->gridResZ = 1;

  int cellCount = set->gridResX * set->gridResZ;
  set->cells =
      (BrushFoliageGridCell *)MemAlloc(sizeof(BrushFoliageGridCell) * cellCount);
  memset(set->cells, 0, sizeof(BrushFoliageGridCell) * cellCount);

  // Pass 1: count per cell.
  for (int i = 0; i < set->count; i++) {
    int cx, cz;
    WorldToCell(set, set->positions[i].x, set->positions[i].z, &cx, &cz);
    set->cells[cz * set->gridResX + cx].capacity++;
  }

  set->gridIndices =
      (int *)MemAlloc(sizeof(int) * (set->count > 0 ? set->count : 1));

  // Prefix-sum offsets; reset count for the fill pass.
  int currentOffset = 0;
  for (int c = 0; c < cellCount; c++) {
    set->cells[c].offset = currentOffset;
    currentOffset += set->cells[c].capacity;
    set->cells[c].count = 0;
  }

  // Pass 2: fill.
  for (int i = 0; i < set->count; i++) {
    int cx, cz;
    WorldToCell(set, set->positions[i].x, set->positions[i].z, &cx, &cz);
    BrushFoliageGridCell *cell = &set->cells[cz * set->gridResX + cx];
    set->gridIndices[cell->offset + cell->count++] = i;
  }
}

// Shared cell-iteration setup: normalized XZ forward + the front-to-back cell
// stepping bounds. Returns false if the set is empty (nothing to cull).
typedef struct {
  float fwdX, fwdZ;
  int startCx, endCx, stepX;
  int startCz, endCz, stepZ;
  float cellRadiusWorld;
} CullWalk;

static bool CullWalkBegin(const BrushFoliageSet *set, Vector3 viewPos,
                          Vector3 viewTarget, float drawDistance, CullWalk *w) {
  if (set->count == 0 || set->cells == NULL) return false;

  float fwdX = viewTarget.x - viewPos.x;
  float fwdZ = viewTarget.z - viewPos.z;
  float fwdLen = sqrtf(fwdX * fwdX + fwdZ * fwdZ);
  if (fwdLen > 0.001f) {
    fwdX /= fwdLen;
    fwdZ /= fwdLen;
  } else {
    fwdX = 0.0f;
    fwdZ = 1.0f;
  }

  int camCx, camCz;
  WorldToCell(set, viewPos.x, viewPos.z, &camCx, &camCz);
  int cellRadius = (int)ceilf(drawDistance / set->cellSize) + 1;

  int minCx = camCx - cellRadius, maxCx = camCx + cellRadius;
  int minCz = camCz - cellRadius, maxCz = camCz + cellRadius;
  if (minCx < 0) minCx = 0;
  if (minCz < 0) minCz = 0;
  if (maxCx >= set->gridResX) maxCx = set->gridResX - 1;
  if (maxCz >= set->gridResZ) maxCz = set->gridResZ - 1;

  w->fwdX = fwdX;
  w->fwdZ = fwdZ;
  w->stepX = (fwdX >= 0.0f) ? 1 : -1;
  w->startCx = (w->stepX == 1) ? minCx : maxCx;
  w->endCx = (w->stepX == 1) ? maxCx : minCx;
  w->stepZ = (fwdZ >= 0.0f) ? 1 : -1;
  w->startCz = (w->stepZ == 1) ? minCz : maxCz;
  w->endCz = (w->stepZ == 1) ? maxCz : minCz;
  w->cellRadiusWorld = set->cellSize * 0.7071067f; // sqrt(0.5)
  return true;
}

// Per-instance visibility: distance^2 (out) + FOV test (matches the donor's
// proven grass cull). A wide ~60-degree half-cone covers a ~50-deg vertical
// FOV with margin; instances within 6 m skip the cone (they wrap around the
// camera in third person). Returns false to skip.
static inline bool InstanceVisible(const BrushFoliageSet *set, int i,
                                   Vector3 viewPos, float fwdX, float fwdZ,
                                   float maxDist2, float pad, float *dist2Out) {
  float dx = set->positions[i].x - viewPos.x;
  float dz = set->positions[i].z - viewPos.z;
  float dist2 = dx * dx + dz * dz;
  if (dist2 > maxDist2) return false;
  if (dist2 >= 36.0f) {
    // `pad` slackens the cone by the instance's extent: the tests are on the
    // BASE point, but a wide/tall canopy is visible before its base enters
    // the 60-degree cone (grass pad ~0, trees pad = their bounds).
    float projDist = dx * fwdX + dz * fwdZ + pad;
    if (projDist < 0.0f) return false;                     // behind camera
    if (projDist * projDist < 0.25f * dist2) return false; // cos(60 deg)^2
  }
  *dist2Out = dist2;
  return true;
}

// Append a transform to the batch buffer for its model variant (bounds-checked).
static inline void BatchAppend(BrushFoliageDrawBatch *b, unsigned char mi,
                               Matrix m) {
  int idx = (mi < b->modelCount) ? mi : 0;
  if (b->count[idx] < b->cap) b->buf[idx][b->count[idx]++] = m;
}

// LOD cross-fade transition widths. The SAME value must drive three things or
// the fade tears: the CPU double-draw overlap band (BrushFoliageCull), the
// outgoing band's height fade-OUT, and the incoming band's height fade-IN (both
// set as shader windows in the draw). One boundary width for near->far (scaled
// to the near range) and one for the longer far->billboard->cull edges.
static inline float BrushFoliageNearTransition(float lodDistance) {
  return (lodDistance * 0.2f < 5.0f) ? (lodDistance * 0.2f) : 5.0f;
}
static inline float BrushFoliageFarTransition(float drawDistance) {
  return (drawDistance * 0.1f < 10.0f) ? (drawDistance * 0.1f) : 10.0f;
}

// Density taper for the OUTER (billboard) tier: past `start`, keep a
// distance-falling fraction of instances so the field thins into the terrain
// instead of ending at full density on a hard line. The keep test is a stable
// hash of the instance's world XZ (0.25 m grid) — the SAME instances drop every
// frame, so the thinning never flickers. Keeps ~THIN_FLOOR at the cull edge.
#define BRUSH_FOLIAGE_THIN_FLOOR 0.18f
static inline bool BrushFoliageThinKeep(float x, float z, float dist,
                                        float start, float drawDistance) {
  if (dist <= start || drawDistance <= start) return true;
  float t = (dist - start) / (drawDistance - start);
  if (t > 1.0f) t = 1.0f;
  t = t * t * (3.0f - 2.0f * t);
  float keepProb = 1.0f - t * (1.0f - BRUSH_FOLIAGE_THIN_FLOOR);
  int xi = (int)floorf(x * 4.0f), zi = (int)floorf(z * 4.0f);
  unsigned int h = (unsigned)xi * 73856093u ^ (unsigned)zi * 19349663u;
  h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
  return (float)(h & 0xffffff) / 16777216.0f < keepProb;
}

void BrushFoliageCull(const BrushFoliageSet *set, Vector3 viewPos,
                      Vector3 viewTarget, float drawDistance, float lodDistance,
                      float billboardDistance, float pad, bool thinning,
                      BrushFoliageDrawBatch *nearB, BrushFoliageDrawBatch *farB,
                      BrushFoliageDrawBatch *bbB) {
  CullWalk w;
  if (!CullWalkBegin(set, viewPos, viewTarget, drawDistance, &w)) return;

  float maxDist2 = drawDistance * drawDistance;
  bool useBB = (bbB != NULL && billboardDistance > 0.0f);
  // Cross-fade overlap widths, per boundary. The draw computes these with the
  // SAME helpers, so the outgoing band's height-fade-out and the incoming band's
  // fade-in line up exactly with this double-draw overlap.
  float tNear = BrushFoliageNearTransition(lodDistance);
  float tFar = BrushFoliageFarTransition(drawDistance);

  for (int cz = w.startCz; cz != w.endCz + w.stepZ; cz += w.stepZ) {
    for (int cx = w.startCx; cx != w.endCx + w.stepX; cx += w.stepX) {
      BrushFoliageGridCell *cell = &set->cells[cz * set->gridResX + cx];
      if (cell->count == 0) continue;

      // Skip the whole cell if its bounding sphere is behind the camera.
      float cwx = set->originX + (cx + 0.5f) * set->cellSize;
      float cwz = set->originZ + (cz + 0.5f) * set->cellSize;
      float projC = (cwx - viewPos.x) * w.fwdX + (cwz - viewPos.z) * w.fwdZ;
      if (projC < -(w.cellRadiusWorld + 4.0f + pad)) continue;

      float dx = cwx - viewPos.x;
      float dz = cwz - viewPos.z;
      float cDist = sqrtf(dx * dx + dz * dz);
      float minDist = cDist - w.cellRadiusWorld;
      float maxDist = cDist + w.cellRadiusWorld;

      // A cell takes the block-copy fast path only if EVERY instance lands in
      // one band (no overlap straddle) AND is safely inside the cull cone. The
      // far band's far edge is the billboard boundary (or the cull edge when the
      // impostor tier is off) — bounding it here keeps the fast path from
      // block-copying instances past drawDistance.
      float farEdge = useBB ? billboardDistance : drawDistance;
      bool fullyNear = (maxDist < lodDistance - tNear);
      bool fullyFar = (minDist >= lodDistance && maxDist < farEdge - tFar);
      bool fullyBB = (useBB && minDist >= billboardDistance && maxDist < drawDistance);
      bool fullyInCone = (cDist < 6.0f) || (projC > 0.0f && projC * projC > 0.36f * (dx * dx + dz * dz));

      if (fullyInCone && (fullyNear || fullyFar || fullyBB)) {
        BrushFoliageDrawBatch *dst = fullyNear ? nearB : (fullyFar ? farB : bbB);
        // Outer tier tapers density into the terrain — unless thinning is off
        // (tree layers: a vanishing tree is a bug, a vanishing tuft a feature).
        bool thin = (dst == bbB) && thinning;
        for (int k = 0; k < cell->count; k++) {
          int i = set->gridIndices[cell->offset + k];
          if (thin && !BrushFoliageThinKeep(set->positions[i].x, set->positions[i].z,
                                            cDist, billboardDistance, drawDistance))
            continue;
          unsigned char mi = set->modelIdx ? set->modelIdx[i] : 0;
          BatchAppend(dst, mi, set->transforms[i]);
        }
      } else {
        for (int k = 0; k < cell->count; k++) {
          int i = set->gridIndices[cell->offset + k];
          float dist2;
          if (!InstanceVisible(set, i, viewPos, w.fwdX, w.fwdZ, maxDist2, pad,
                               &dist2))
            continue;
          float dist = sqrtf(dist2);
          unsigned char mi = set->modelIdx ? set->modelIdx[i] : 0;
          // Bands overlap by their transition width so the outgoing/incoming
          // meshes cross-fade (double-drawn there, each height-faded).
          if (dist < lodDistance) {
            BatchAppend(nearB, mi, set->transforms[i]);
          }
          if (dist >= lodDistance - tNear && (!useBB || dist < billboardDistance)) {
            BatchAppend(farB, mi, set->transforms[i]);
          }
          if (useBB && dist >= billboardDistance - tFar &&
              (!thinning ||
               BrushFoliageThinKeep(set->positions[i].x, set->positions[i].z,
                                    dist, billboardDistance, drawDistance))) {
            BatchAppend(bbB, mi, set->transforms[i]);
          }
        }
      }
    }
  }
}

void BrushFoliageSetCleanup(BrushFoliageSet *set) {
  if (!set) return;
  if (set->cells) { MemFree(set->cells); set->cells = NULL; }
  if (set->gridIndices) { MemFree(set->gridIndices); set->gridIndices = NULL; }
  if (set->transforms) { MemFree(set->transforms); set->transforms = NULL; }
  if (set->positions) { MemFree(set->positions); set->positions = NULL; }
  if (set->modelIdx) { MemFree(set->modelIdx); set->modelIdx = NULL; }
  set->count = 0;
}

// --- LOD mesh builders (main thread) ----------------------------------------

Mesh BrushFoliageBuildLODMesh(Mesh source, float keepRatio) {
  Mesh lod = {0};
  lod.vertexCount = source.vertexCount;
  lod.triangleCount = source.triangleCount;

  lod.vertices = (float *)MemAlloc(sizeof(float) * source.vertexCount * 3);
  memcpy(lod.vertices, source.vertices, sizeof(float) * source.vertexCount * 3);
  if (source.texcoords) {
    lod.texcoords = (float *)MemAlloc(sizeof(float) * source.vertexCount * 2);
    memcpy(lod.texcoords, source.texcoords, sizeof(float) * source.vertexCount * 2);
  }
  if (source.normals) {
    lod.normals = (float *)MemAlloc(sizeof(float) * source.vertexCount * 3);
    memcpy(lod.normals, source.normals, sizeof(float) * source.vertexCount * 3);
  }
  if (source.colors) {
    lod.colors = (unsigned char *)MemAlloc(sizeof(unsigned char) * source.vertexCount * 4);
    memcpy(lod.colors, source.colors, sizeof(unsigned char) * source.vertexCount * 4);
  }
  if (source.indices) {
    lod.indices = (unsigned short *)MemAlloc(sizeof(unsigned short) * source.triangleCount * 3);
    memcpy(lod.indices, source.indices, sizeof(unsigned short) * source.triangleCount * 3);
  }

  BrushMeshDecimate(&lod, keepRatio);
  UploadMesh(&lod, false);
  return lod;
}

Mesh BrushFoliageBuildLODMeshTarget(Mesh source, float targetRatio, int maxTris) {
  float keepRatio = targetRatio;
  if (source.triangleCount > 0) {
    float capRatio = (float)maxTris / (float)source.triangleCount;
    if (capRatio < keepRatio) keepRatio = capRatio;
  }
  if (keepRatio < 0.02f) keepRatio = 0.02f; // safety floor
  return BrushFoliageBuildLODMesh(source, keepRatio);
}

Mesh BrushFoliageBuildBillboardMesh(Mesh source) {
  // Bounding box of the source mesh -> card size.
  float minX = 1e9f, maxX = -1e9f;
  float minY = 1e9f, maxY = -1e9f;
  float minZ = 1e9f, maxZ = -1e9f;
  for (int i = 0; i < source.vertexCount; i++) {
    float x = source.vertices[i * 3 + 0];
    float y = source.vertices[i * 3 + 1];
    float z = source.vertices[i * 3 + 2];
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
    if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
  }
  // Half-width from the wider horizontal spread; scale to 85% to compensate for
  // billboards lacking perspective foreshortening vs. the real 3D mesh.
  float w = fmaxf(maxX - minX, maxZ - minZ) * 0.5f * 0.85f;
  float h = (maxY - minY) * 0.85f;
  return BrushFoliageBuildBillboardMeshWH(w, h);
}

// Same card from explicit half-width/height — used by the impostor bake, which
// frames the UNION of a variant's submeshes (the atlas and the card must agree).
Mesh BrushFoliageBuildBillboardMeshWH(float w, float h) {
  Mesh billboard = {0};
  if (w <= 0.0f) w = 0.5f;
  if (h <= 0.0f) h = 1.0f;

  billboard.vertexCount = 8;
  billboard.triangleCount = 4;
  billboard.vertices = (float *)MemAlloc(sizeof(float) * 8 * 3);
  billboard.texcoords = (float *)MemAlloc(sizeof(float) * 8 * 2);
  billboard.normals = (float *)MemAlloc(sizeof(float) * 8 * 3);
  billboard.colors = (unsigned char *)MemAlloc(sizeof(unsigned char) * 8 * 4);
  billboard.indices = (unsigned short *)MemAlloc(sizeof(unsigned short) * 4 * 3);

  // Vertex-color alpha carries height (0 base, 255 tip); RGB white so the
  // texture isn't tinted. Bottom verts {0,1,4,5}, top verts {2,3,6,7}.
  int bottomVerts[4] = {0, 1, 4, 5};
  for (int i = 0; i < 4; i++) {
    int idx = bottomVerts[i] * 4;
    billboard.colors[idx + 0] = 255; billboard.colors[idx + 1] = 255;
    billboard.colors[idx + 2] = 255; billboard.colors[idx + 3] = 0;
  }
  int topVerts[4] = {2, 3, 6, 7};
  for (int i = 0; i < 4; i++) {
    int idx = topVerts[i] * 4;
    billboard.colors[idx + 0] = 255; billboard.colors[idx + 1] = 255;
    billboard.colors[idx + 2] = 255; billboard.colors[idx + 3] = 255;
  }

  // Quad 1 (spans X). 0 bl, 1 br, 2 tr, 3 tl.
  billboard.vertices[0] = -w; billboard.vertices[1] = 0.0f; billboard.vertices[2] = 0.0f;
  billboard.texcoords[0] = 0.0f; billboard.texcoords[1] = 1.0f;
  billboard.vertices[3] = w; billboard.vertices[4] = 0.0f; billboard.vertices[5] = 0.0f;
  billboard.texcoords[2] = 1.0f; billboard.texcoords[3] = 1.0f;
  billboard.vertices[6] = w; billboard.vertices[7] = h; billboard.vertices[8] = 0.0f;
  billboard.texcoords[4] = 1.0f; billboard.texcoords[5] = 0.0f;
  billboard.vertices[9] = -w; billboard.vertices[10] = h; billboard.vertices[11] = 0.0f;
  billboard.texcoords[6] = 0.0f; billboard.texcoords[7] = 0.0f;

  // Quad 2 (spans Z). 4 bl, 5 br, 6 tr, 7 tl.
  billboard.vertices[12] = 0.0f; billboard.vertices[13] = 0.0f; billboard.vertices[14] = -w;
  billboard.texcoords[8] = 0.0f; billboard.texcoords[9] = 1.0f;
  billboard.vertices[15] = 0.0f; billboard.vertices[16] = 0.0f; billboard.vertices[17] = w;
  billboard.texcoords[10] = 1.0f; billboard.texcoords[11] = 1.0f;
  billboard.vertices[18] = 0.0f; billboard.vertices[19] = h; billboard.vertices[20] = w;
  billboard.texcoords[12] = 1.0f; billboard.texcoords[13] = 0.0f;
  billboard.vertices[21] = 0.0f; billboard.vertices[22] = h; billboard.vertices[23] = -w;
  billboard.texcoords[14] = 0.0f; billboard.texcoords[15] = 0.0f;

  // Soft radial normals (outward from the center axis, biased upward from root
  // to tip) so the cross-quad shades like a fluffy volume, not two flat cards.
  for (int i = 0; i < 8; i++) {
    float vx = billboard.vertices[i * 3 + 0];
    float vy = billboard.vertices[i * 3 + 1];
    float vz = billboard.vertices[i * 3 + 2];
    float nx = vx, nz = vz;
    float heightRatio = vy / fmaxf(h, 0.001f);
    float ny = h * (0.2f + 0.8f * heightRatio);
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (len > 0.0f) { nx /= len; ny /= len; nz /= len; }
    else { ny = 1.0f; }
    billboard.normals[i * 3 + 0] = nx;
    billboard.normals[i * 3 + 1] = ny;
    billboard.normals[i * 3 + 2] = nz;
  }

  billboard.indices[0] = 0; billboard.indices[1] = 1; billboard.indices[2] = 2;
  billboard.indices[3] = 0; billboard.indices[4] = 2; billboard.indices[5] = 3;
  billboard.indices[6] = 4; billboard.indices[7] = 5; billboard.indices[8] = 6;
  billboard.indices[9] = 4; billboard.indices[10] = 6; billboard.indices[11] = 7;

  // The impostor texture is rendered base-down (uv.y=0 = base). The quad was
  // built with base verts at uv.y=1, so flip V to avoid an upside-down card.
  for (int i = 0; i < 8; i++)
    billboard.texcoords[i * 2 + 1] = 1.0f - billboard.texcoords[i * 2 + 1];

  UploadMesh(&billboard, false);
  return billboard;
}

// --- Zero-asset procedural defaults + shader --------------------------------

// Deterministic [0,1) LCG for blade scatter within a patch (donor parity).
static float PatchRand(unsigned int *s) {
  *s = *s * 1103515245u + 12345u;
  return (float)((*s >> 16) & 0x7fff) / 32767.0f;
}

Mesh BrushFoliageMakeGrassPatch(int blades, float radius, float height) {
  if (blades < 1) blades = 1;
  if (radius <= 0.0f) radius = 0.45f;
  if (height <= 0.0f) height = 0.24f;

  Mesh m = {0};
  m.vertexCount = blades * 5;
  m.triangleCount = blades * 3;
  m.vertices = (float *)MemAlloc(sizeof(float) * m.vertexCount * 3);
  m.texcoords = (float *)MemAlloc(sizeof(float) * m.vertexCount * 2);
  m.normals = (float *)MemAlloc(sizeof(float) * m.vertexCount * 3);
  m.colors = (unsigned char *)MemAlloc(sizeof(unsigned char) * m.vertexCount * 4);
  m.indices = (unsigned short *)MemAlloc(sizeof(unsigned short) * m.triangleCount * 3);

  unsigned int seed = 1337u;
  const float TAU = 6.2831853f;

  for (int i = 0; i < blades; i++) {
    // Blade base: uniform within the patch circle (sqrt for even area).
    float theta = PatchRand(&seed) * TAU;
    float r = radius * sqrtf(PatchRand(&seed));
    float bx = r * cosf(theta), bz = r * sinf(theta);

    float bh = height * (0.70f + 0.60f * PatchRand(&seed)); // 70..130% height
    float bw = 0.005f * (0.60f + 0.60f * PatchRand(&seed)); // ~0.6..1.2 cm wide

    float yaw = theta + (PatchRand(&seed) - 0.5f) * 0.6f;   // mostly outward
    float fwdX = cosf(yaw), fwdZ = sinf(yaw);
    float wX = -sinf(yaw) * bw, wZ = cosf(yaw) * bw;

    float bend = (0.25f + 0.45f * PatchRand(&seed)) * bh;   // outward curve
    float sink = bend * 0.25f;                              // gravity droop

    int v = i * 5;
    // v0 base-left, v1 base-right, v2 mid-left, v3 mid-right, v4 tip.
    float px[5] = {bx - wX, bx + wX, bx - wX * 0.8f + fwdX * bend * 0.3f,
                   bx + wX * 0.8f + fwdX * bend * 0.3f, bx + fwdX * bend};
    float py[5] = {0.0f, 0.0f, bh * 0.5f - sink * 0.2f, bh * 0.5f - sink * 0.2f,
                   bh - sink};
    float pz[5] = {bz - wZ, bz + wZ, bz - wZ * 0.8f + fwdZ * bend * 0.3f,
                   bz + wZ * 0.8f + fwdZ * bend * 0.3f, bz + fwdZ * bend};
    // UVs: one of three vertical texture strips, narrowing to the tip.
    float uOff = (float)(i % 3) * 0.33f, uW = 0.33f;
    float uu[5] = {uOff, uOff + uW, uOff + uW * 0.1f, uOff + uW * 0.9f, uOff + uW * 0.5f};
    float vv[5] = {0.0f, 0.0f, 0.5f, 0.5f, 1.0f};
    unsigned char av[5] = {0, 0, 127, 127, 255}; // height in alpha (wind sway)

    float nX = sinf(yaw), nZ = cosf(yaw), nY = 0.4f;
    float nl = sqrtf(nX * nX + nY * nY + nZ * nZ);
    nX /= nl; nY /= nl; nZ /= nl;

    for (int k = 0; k < 5; k++) {
      int vi = v + k;
      m.vertices[vi * 3 + 0] = px[k];
      m.vertices[vi * 3 + 1] = py[k];
      m.vertices[vi * 3 + 2] = pz[k];
      m.texcoords[vi * 2 + 0] = uu[k];
      m.texcoords[vi * 2 + 1] = vv[k];
      m.normals[vi * 3 + 0] = nX;
      m.normals[vi * 3 + 1] = nY;
      m.normals[vi * 3 + 2] = nZ;
      m.colors[vi * 4 + 0] = 255; m.colors[vi * 4 + 1] = 255;
      m.colors[vi * 4 + 2] = 255; m.colors[vi * 4 + 3] = av[k];
    }

    int t = i * 9; // 3 tris * 3 indices
    m.indices[t + 0] = v + 0; m.indices[t + 1] = v + 1; m.indices[t + 2] = v + 2;
    m.indices[t + 3] = v + 1; m.indices[t + 4] = v + 3; m.indices[t + 5] = v + 2;
    m.indices[t + 6] = v + 2; m.indices[t + 7] = v + 3; m.indices[t + 8] = v + 4;
  }

  UploadMesh(&m, false);
  return m;
}

Mesh BrushFoliageMakeTuft(int blades, float height, float width) {
  if (blades < 1) blades = 1;
  if (height <= 0.0f) height = 0.4f;
  if (width <= 0.0f) width = 0.12f;

  Mesh m = {0};
  m.vertexCount = blades * 4;
  m.triangleCount = blades * 2;
  m.vertices = (float *)MemAlloc(sizeof(float) * m.vertexCount * 3);
  m.texcoords = (float *)MemAlloc(sizeof(float) * m.vertexCount * 2);
  m.normals = (float *)MemAlloc(sizeof(float) * m.vertexCount * 3);
  m.colors = (unsigned char *)MemAlloc(sizeof(unsigned char) * m.vertexCount * 4);
  m.indices = (unsigned short *)MemAlloc(sizeof(unsigned short) * m.triangleCount * 3);

  const float half = width * 0.5f;
  const float tipTaper = 0.5f; // narrow the blade toward the tip

  for (int p = 0; p < blades; p++) {
    float angle = (float)p * (3.14159265f / (float)blades);
    float dx = cosf(angle), dz = sinf(angle);
    int v = p * 4;

    // v+0 base-left, v+1 base-right, v+2 tip-right, v+3 tip-left.
    float px[4] = {-dx * half, dx * half, dx * half * tipTaper, -dx * half * tipTaper};
    float pz[4] = {-dz * half, dz * half, dz * half * tipTaper, -dz * half * tipTaper};
    float py[4] = {0.0f, 0.0f, height, height};
    float u[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    float vv[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // v: base 0, tip 1

    for (int c = 0; c < 4; c++) {
      int vi = v + c;
      m.vertices[vi * 3 + 0] = px[c];
      m.vertices[vi * 3 + 1] = py[c];
      m.vertices[vi * 3 + 2] = pz[c];
      m.texcoords[vi * 2 + 0] = u[c];
      m.texcoords[vi * 2 + 1] = vv[c];

      // Soft radial normal: outward from the blade plane + upward bias to the
      // tip, so a tuft shades like a soft volume rather than flat cards.
      float ny = 0.35f + 0.65f * vv[c];
      float nx = dz, nz = -dx; // perpendicular to the blade direction (XZ)
      float len = sqrtf(nx * nx + ny * ny + nz * nz);
      m.normals[vi * 3 + 0] = nx / len;
      m.normals[vi * 3 + 1] = ny / len;
      m.normals[vi * 3 + 2] = nz / len;

      unsigned char a = (vv[c] > 0.5f) ? 255 : 0; // height in alpha (wind sway)
      m.colors[vi * 4 + 0] = 255;
      m.colors[vi * 4 + 1] = 255;
      m.colors[vi * 4 + 2] = 255;
      m.colors[vi * 4 + 3] = a;
    }

    int t = p * 6;
    m.indices[t + 0] = v + 0; m.indices[t + 1] = v + 1; m.indices[t + 2] = v + 2;
    m.indices[t + 3] = v + 0; m.indices[t + 4] = v + 2; m.indices[t + 5] = v + 3;
  }

  UploadMesh(&m, false);
  return m;
}

Texture2D BrushFoliageMakeGradientTex(Color base, Color tip) {
  // direction 0 = vertical; start (base) at v=0 (top), end (tip) at v=1.
  Image img = GenImageGradientLinear(4, 64, 0, base, tip);
  Texture2D tex = LoadTextureFromImage(img);
  UnloadImage(img);
  GenTextureMipmaps(&tex);
  SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
  return tex;
}

Shader BrushFoliageLoadShader(void) {
  Shader s = LoadShader(BrushEnginePath("engine/shaders/foliage.vs"),
                        BrushEnginePath("engine/shaders/foliage.fs"));
  // raylib instancing contract: mvp = view*proj, per-instance model comes from
  // the `instanceTransform` attribute (fed into SHADER_LOC_MATRIX_MODEL).
  s.locs[SHADER_LOC_MATRIX_MVP] = GetShaderLocation(s, "mvp");
  s.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(s, "viewPos");
  s.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(s, "instanceTransform");
  // Map slots for the material textures: diffuse -> texture0, and the billboard
  // impostor's baked NORMAL atlas on the NORMAL slot -> texture2 (raylib binds
  // material map i to unit i and sets shader.locs[SHADER_LOC_MAP_DIFFUSE + i]).
  s.locs[SHADER_LOC_MAP_DIFFUSE] = GetShaderLocation(s, "texture0");
  s.locs[SHADER_LOC_MAP_NORMAL] = GetShaderLocation(s, "texture2");
  return s;
}

// --- Foliage system (layers streamed over the chunk world) ------------------

typedef struct BrushFoliageLayer {
  BrushFoliageLayerConfig cfg;
  int meshCount;   // model variants (>=1; a meadow mixes several meshes)
  // Per-variant SUBMESHES (a tree = bark + cutout leaves): they share the
  // variant's instance-transform batch and draw as one instanced call each.
  // Grass/tuft variants have subCount 1 — identical to the old single-mesh
  // path. The billboard impostor stays PER VARIANT (all submeshes bake into
  // one atlas pair).
  int subCount[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  Mesh nearMesh[BRUSH_FOLIAGE_MODELS_PER_LAYER][BRUSH_FOLIAGE_SUBMESHES]; // shared model mesh or tuft
  Mesh farMesh[BRUSH_FOLIAGE_MODELS_PER_LAYER][BRUSH_FOLIAGE_SUBMESHES];  // decimated LOD, owned
  bool hasFar[BRUSH_FOLIAGE_MODELS_PER_LAYER][BRUSH_FOLIAGE_SUBMESHES];
  Material material[BRUSH_FOLIAGE_MODELS_PER_LAYER][BRUSH_FOLIAGE_SUBMESHES];
  // Billboard impostor band (opt-in): a crossed-quad + a texture baked once from
  // the clump, drawn beyond billboardDistance in place of the far mesh.
  Mesh billboardMesh[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  Material billboardMat[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  RenderTexture2D impostorRT[BRUSH_FOLIAGE_MODELS_PER_LAYER];    // albedo atlas
  RenderTexture2D impostorNrmRT[BRUSH_FOLIAGE_MODELS_PER_LAYER]; // normal atlas
  bool hasImpostor[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  float billboardDistance; // 0 = no billboard band (2-tier)
  float cullPad; // worst-case instance extent (m): widens the cull cone +
                 // chunk frustum box so tall/wide models never pop at edges
  BrushFoliageDrawBatch nearB, farB, bbB; // per-model visible buffers (main thread)
  int lastNear, lastFar; // visible counts from the last draw (editor stats)
} BrushFoliageLayer;

struct BrushFoliage {
  Shader shader;
  Mesh defaultTuft;
  Texture2D defaultGradient;
  BrushFoliageLayer layers[BRUSH_FOLIAGE_MAX_LAYERS];
  int layerCount;
  Vector2 windDir;
  float windStrength;
  BrushWorld *world;
  float time;
  float distanceScale;
  BrushFoliageQuality quality;
  float qualityScale;  // draw-distance multiplier from the preset
  bool qualityShadows; // whether foliage samples the shadow map
  int locTime, locWindDir, locWindStr, locFadeStart, locFadeEnd;
  int locFadeNearStart, locFadeNearEnd;
  int locMacroLow, locMacroHigh, locGrassTint, locAlphaCutoff;
  int locImpostor;     // 1 = billboard draw (albedo atlas + baked-normal lighting)
  int locImpostorBake; // bake pass: 1 = emit albedo, 2 = emit encoded normal
  bool impostor; // BRUSH_FOLIAGE_IMPOSTOR: bake a billboard far-tier per variant
};

// One chunk's scattered instances, one set per layer. The opaque world handle.
typedef struct {
  BrushFoliageSet sets[BRUSH_FOLIAGE_MAX_LAYERS];
  int setCount;
} FoliageChunkHandle;

BrushFoliage *BrushFoliageCreate(void) {
  BrushFoliage *f = MemAlloc(sizeof(BrushFoliage));
  memset(f, 0, sizeof(*f));
  f->shader = BrushFoliageLoadShader();
  f->defaultTuft = BrushFoliageMakeGrassPatch(40, 0.25f, 0.24f);
  f->defaultGradient = BrushFoliageMakeGradientTex((Color){52, 78, 36, 255},
                                                   (Color){150, 180, 84, 255});
  f->windDir = (Vector2){0.9578f, 0.2873f}; // normalized (1, 0.3)
  f->windStrength = 1.0f;
  f->distanceScale = 1.0f;
  BrushFoliageQuality q = BRUSH_FOLIAGE_HIGH;
  const char *qe = getenv("BRUSH_FOLIAGE_QUALITY");
  if (qe) {
    int v = atoi(qe);
    q = (v <= 0) ? BRUSH_FOLIAGE_LOW : (v >= 2 ? BRUSH_FOLIAGE_HIGH : BRUSH_FOLIAGE_MED);
  }
  BrushFoliageSetQuality(f, q);
  f->locTime = GetShaderLocation(f->shader, "uTime");
  f->locWindDir = GetShaderLocation(f->shader, "uWindDirection");
  f->locWindStr = GetShaderLocation(f->shader, "uWindStrength");
  f->locFadeStart = GetShaderLocation(f->shader, "uFadeStart");
  f->locFadeEnd = GetShaderLocation(f->shader, "uFadeEnd");
  f->locFadeNearStart = GetShaderLocation(f->shader, "uFadeNearStart");
  f->locFadeNearEnd = GetShaderLocation(f->shader, "uFadeNearEnd");
  f->locMacroLow = GetShaderLocation(f->shader, "uMacroLow");
  f->locMacroHigh = GetShaderLocation(f->shader, "uMacroHigh");
  f->locGrassTint = GetShaderLocation(f->shader, "uGrassTint");
  f->locAlphaCutoff = GetShaderLocation(f->shader, "uAlphaCutoff");
  f->locImpostor = GetShaderLocation(f->shader, "uImpostor");
  f->locImpostorBake = GetShaderLocation(f->shader, "uImpostorBake");
  // Billboard impostor far-tier: ON by default (day/night-correct, ~+21% under
  // heavy grass load); set BRUSH_FOLIAGE_IMPOSTOR=0 to disable.
  const char *impEnv = getenv("BRUSH_FOLIAGE_IMPOSTOR");
  f->impostor = !(impEnv && impEnv[0] == '0');
  return f;
}

// One bake pass: render the clump into `rt` with the given uImpostorBake mode
// (1 = albedo, 2 = encoded normal) through a manual orthographic front view.
// The manual rlOrtho matters: raylib's BeginMode3D derives aspect from the
// SCREEN, not the bound render target, which would distort a non-square atlas.
static void BrushFoliageBakePass(BrushFoliage *f, BrushFoliageLayer *L, int m,
                                 RenderTexture2D rt, float bw, float bh, float D,
                                 float mode) {
  float zero = 0.0f, cutoff = 0.3f, vp[3] = {0, 0, D};
  BeginTextureMode(rt);
  ClearBackground((Color){0, 0, 0, 0}); // transparent atlas
  rlDrawRenderBatchActive();
  rlMatrixMode(RL_PROJECTION);
  rlPushMatrix();
  rlLoadIdentity();
  rlOrtho(-bw, bw, 0.0, bh, 0.01, 2.0 * D); // exact framing -> no aspect distortion
  rlMatrixMode(RL_MODELVIEW);
  rlLoadIdentity();
  rlTranslatef(0.0f, 0.0f, -D); // look down -Z from +Z, no rotation

  SetShaderValue(f->shader, f->locImpostorBake, &mode, SHADER_UNIFORM_FLOAT);
  SetShaderValue(f->shader, f->locTime, &zero, SHADER_UNIFORM_FLOAT);
  SetShaderValue(f->shader, f->locWindStr, &zero, SHADER_UNIFORM_FLOAT);
  // The vertex height-fade MUST be neutralised for the bake: with uFadeEnd
  // left at its default 0, fadeNorm = clamp(D / max(0, 0.001)) = 1 and
  // heightScale collapses to ZERO — the model bakes at zero height and the
  // atlas comes out BLANK (billboards drawn but invisible: "trees vanish at
  // distance", "grass respawns mid-range"). Push the fade window far out and
  // disable the near fade-in so the bake sees the full-height model.
  float farEnd = 1e6f;
  SetShaderValue(f->shader, f->locFadeStart, &zero, SHADER_UNIFORM_FLOAT);
  SetShaderValue(f->shader, f->locFadeEnd, &farEnd, SHADER_UNIFORM_FLOAT);
  SetShaderValue(f->shader, f->locFadeNearStart, &zero, SHADER_UNIFORM_FLOAT);
  SetShaderValue(f->shader, f->locFadeNearEnd, &zero, SHADER_UNIFORM_FLOAT);
  SetShaderValue(f->shader, f->locAlphaCutoff, &cutoff, SHADER_UNIFORM_FLOAT);
  SetShaderValue(f->shader, f->shader.locs[SHADER_LOC_VECTOR_VIEW], vp, SHADER_UNIFORM_VEC3);

  // Every submesh of the variant renders into the same atlas, each with its
  // own albedo (bark and leaves keep their textures in the impostor).
  Material bakeMat = LoadMaterialDefault();
  bakeMat.shader = f->shader;
  bakeMat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  Matrix idn = MatrixIdentity();
  rlDisableBackfaceCulling();
  for (int s = 0; s < L->subCount[m]; s++) {
    bakeMat.maps[MATERIAL_MAP_DIFFUSE].texture =
        L->material[m][s].maps[MATERIAL_MAP_DIFFUSE].texture;
    DrawMeshInstanced(L->nearMesh[m][s], bakeMat, &idn, 1);
  }
  rlDrawRenderBatchActive();

  rlMatrixMode(RL_PROJECTION);
  rlPopMatrix();
  rlMatrixMode(RL_MODELVIEW);
  rlLoadIdentity();
  EndTextureMode();

  bakeMat.shader = (Shader){0}; // shared, not owned
  bakeMat.maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){0};
  UnloadMaterial(bakeMat);
  SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
}

// Bake one grass clump into a billboard impostor: two atlases — ALBEDO and a
// per-texel NORMAL map — captured from the NEAR mesh, framed to match
// BrushFoliageBuildBillboardMesh's cross-quad so the quad's [0,1] UVs map the
// clump 1:1. Grass is only ever seen from a narrow, near-horizontal pitch, so a
// single view (reused on both cross planes) reads convincingly at distance — no
// octahedral angle atlas. Both channels are LIGHT-INDEPENDENT, so the billboard
// is lit live at draw with the current sun (see foliage.fs) and tracks day/night
// with no re-bake and no sudden colour shift. One-time cost.
static void BrushFoliageBakeImpostor(BrushFoliage *f, BrushFoliageLayer *L,
                                     int m) {
  // Frame the UNION of the variant's submeshes (bark + canopy together).
  float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f, maxY = 0.0f;
  int verts = 0;
  for (int s = 0; s < L->subCount[m]; s++) {
    Mesh mesh = L->nearMesh[m][s];
    if (!mesh.vertices || mesh.vertexCount <= 0) continue;
    verts += mesh.vertexCount;
    for (int v = 0; v < mesh.vertexCount; v++) {
      float x = mesh.vertices[v * 3], y = mesh.vertices[v * 3 + 1], z = mesh.vertices[v * 3 + 2];
      if (x < minX) minX = x; if (x > maxX) maxX = x;
      if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
      if (y > maxY) maxY = y;
    }
  }
  if (verts <= 0) return;
  // Match BuildBillboardMesh's framing (85% of the wider horizontal spread).
  float bw = fmaxf(maxX - minX, maxZ - minZ) * 0.5f * 0.85f;
  float bh = maxY * 0.85f;
  if (bw <= 0.001f) bw = 0.5f;
  if (bh <= 0.001f) bh = 1.0f;

  // Atlas height: grass keeps the tuned 256; TREE variants scale with the
  // real mesh height (a 15 m canopy at 256 px smears — racer sized impostors
  // by height and filtered anisotropically for the same reason).
  int rtH = 256;
  if (L->cfg.tree) {
    rtH = (int)lroundf(64.0f * maxY);
    if (rtH < 256) rtH = 256;
    if (rtH > 512) rtH = 512;
  }
  int rtW = (int)lroundf((float)rtH * (2.0f * bw) / bh);
  if (rtW < 32) rtW = 32; if (rtW > 1024) rtW = 1024;
  RenderTexture2D albedoRT = LoadRenderTexture(rtW, rtH);
  RenderTexture2D nrmRT = LoadRenderTexture(rtW, rtH);
  if (albedoRT.id == 0 || nrmRT.id == 0) return;

  float D = bw + bh + 1.0f; // ortho eye distance (rotation-free front view)
  BrushFoliageBakePass(f, L, m, albedoRT, bw, bh, D, 1.0f); // albedo
  BrushFoliageBakePass(f, L, m, nrmRT, bw, bh, D, 2.0f);    // normal
  if (L->cfg.tree) {
    // Tree billboards live at 90-350 m — heavy minification. Racer's fix
    // (trees.c): mips + anisotropic filtering, or the atlas shimmers into
    // pixel dots at distance. Grass atlases stay bilinear (near-constant
    // on-screen size across their short band).
    GenTextureMipmaps(&albedoRT.texture);
    GenTextureMipmaps(&nrmRT.texture);
    SetTextureFilter(albedoRT.texture, TEXTURE_FILTER_ANISOTROPIC_8X);
    SetTextureFilter(nrmRT.texture, TEXTURE_FILTER_ANISOTROPIC_8X);
  }
  float zero = 0.0f;
  SetShaderValue(f->shader, f->locImpostorBake, &zero, SHADER_UNIFORM_FLOAT); // back to draw mode

  // BRUSH_FOLIAGE_DEBUG: dump the atlases so "billboards drawn but invisible"
  // is diagnosable by looking at the texture instead of guessing.
  if (getenv("BRUSH_FOLIAGE_DEBUG")) {
    Image a = LoadImageFromTexture(albedoRT.texture);
    ExportImage(a, TextFormat("impostor_m%d.png", m));
    UnloadImage(a);
  }

  L->impostorRT[m] = albedoRT;
  L->impostorNrmRT[m] = nrmRT;
  // Cross-quad sized from the same union bounds the atlas was framed with
  // (the [0,1] UVs must map the atlas 1:1).
  L->billboardMesh[m] = BrushFoliageBuildBillboardMeshWH(bw, bh);
  L->billboardMat[m] = LoadMaterialDefault();
  L->billboardMat[m].shader = f->shader;
  L->billboardMat[m].maps[MATERIAL_MAP_DIFFUSE].texture = albedoRT.texture;
  L->billboardMat[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  L->billboardMat[m].maps[MATERIAL_MAP_NORMAL].texture = nrmRT.texture; // -> texture2
  L->hasImpostor[m] = true;
}

int BrushFoliageAddLayer(BrushFoliage *f, const BrushFoliageLayerConfig *cfg) {
  if (f->layerCount >= BRUSH_FOLIAGE_MAX_LAYERS) return -1;
  BrushFoliageLayer *L = &f->layers[f->layerCount];
  L->cfg = *cfg;
  if (L->cfg.density <= 0.0f) L->cfg.density = L->cfg.tree ? 0.004f : 1.0f;
  // Tree-scale distance defaults (spec: 50 near -> 90 billboard -> 350 cull).
  if (L->cfg.drawDistance <= 0.0f) L->cfg.drawDistance = L->cfg.tree ? 350.0f : 80.0f;
  if (L->cfg.lodDistance <= 0.0f)
    L->cfg.lodDistance = L->cfg.tree ? fminf(50.0f, L->cfg.drawDistance * 0.15f)
                                     : L->cfg.drawDistance * 0.4f;
  if (L->cfg.scale <= 0.0f) L->cfg.scale = 1.0f;
  if (L->cfg.farKeepRatio <= 0.0f) L->cfg.farKeepRatio = 0.4f;
  if (L->cfg.macroLow.x == 0 && L->cfg.macroLow.y == 0 && L->cfg.macroLow.z == 0)
    L->cfg.macroLow = (Vector3){0.75f, 0.82f, 0.55f};
  if (L->cfg.macroHigh.x == 0 && L->cfg.macroHigh.y == 0 && L->cfg.macroHigh.z == 0)
    L->cfg.macroHigh = (Vector3){1.05f, 1.05f, 0.78f};
  if (L->cfg.tint.x == 0 && L->cfg.tint.y == 0 && L->cfg.tint.z == 0)
    L->cfg.tint = (Vector3){1, 1, 1};
  // F3 (prototype default): zero-asset/procedural grass greens the terrain so a
  // meadow reads as a continuous field past the 3D blades; imported models
  // (meshCount > 0 — rocks, trees) don't tint the ground. Explicit groundStrength
  // wins. TODO: promote to an authored per-layer field (scene + editor UI).
  if (L->cfg.groundStrength <= 0.0f && cfg->meshCount <= 0) {
    L->cfg.groundColor = (Vector3){0.28f, 0.36f, 0.16f};
    L->cfg.groundStrength = 0.65f;
  }

  // Model palette: build near + far LOD meshes and a material per variant
  // SUBMESH. An empty palette (or a variant with no mesh) falls back to the
  // procedural tuft; a submesh with no albedo falls back to the gradient.
  int mc = cfg->meshCount;
  if (mc <= 0) mc = 1;
  if (mc > BRUSH_FOLIAGE_MODELS_PER_LAYER) mc = BRUSH_FOLIAGE_MODELS_PER_LAYER;
  L->meshCount = mc;
  float layerExt = 0.6f; // worst-case variant extent (m, model units x variant scale)
  for (int m = 0; m < mc; m++) {
    int sc = (cfg->meshCount > 0) ? cfg->subCount[m] : 0;
    if (sc <= 0) sc = 1;
    if (sc > BRUSH_FOLIAGE_SUBMESHES) sc = BRUSH_FOLIAGE_SUBMESHES;
    bool fromModel =
        (cfg->meshCount > 0 && cfg->meshes[m][0].vertexCount > 0);
    L->subCount[m] = fromModel ? sc : 1;

    // Ground the provided model: recentre on XZ and drop the base to Y=0,
    // baking ONE offset from the UNION of the variant's submeshes into all of
    // them (grounding each submesh separately would sink a trunk whose canopy
    // submesh starts above the base, and split the tree apart). Baking once —
    // rather than offsetting per instance — means the near mesh and the LOD
    // built from it share the same grounded base, so nothing jumps at the LOD
    // boundary. Idempotent: re-running subtracts ~0 and skips the GPU update.
    if (fromModel) {
      float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, minZ = 1e9f,
            maxZ = -1e9f;
      for (int s = 0; s < L->subCount[m]; s++) {
        Mesh sm = cfg->meshes[m][s];
        if (!sm.vertices) continue;
        for (int v = 0; v < sm.vertexCount; v++) {
          float x = sm.vertices[v * 3], y = sm.vertices[v * 3 + 1], z = sm.vertices[v * 3 + 2];
          if (x < minX) minX = x; if (x > maxX) maxX = x;
          if (y < minY) minY = y; if (y > maxY) maxY = y;
          if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
        }
      }
      // Footprint radius (union XZ half-extent, model units) for the scatter's
      // footprint-aware grounding + road margin. Extents are shift-invariant,
      // so computing from the pre-grounding bounds is exact.
      L->cfg.baseRadius[m] =
          fmaxf(maxX - minX, maxZ - minZ) * 0.5f;
      // Worst-case extent (radius or height) for the layer cull pad, at this
      // variant's authored scale.
      float vs = (cfg->meshScale[m] > 0.0f) ? cfg->meshScale[m] : 1.0f;
      float ext = fmaxf(L->cfg.baseRadius[m], maxY - minY) * vs;
      if (ext > layerExt) layerExt = ext;
      float ox = (minX + maxX) * 0.5f, oy = minY, oz = (minZ + maxZ) * 0.5f;
      if (fabsf(ox) > 1e-4f || fabsf(oy) > 1e-4f || fabsf(oz) > 1e-4f) {
        for (int s = 0; s < L->subCount[m]; s++) {
          Mesh sm = cfg->meshes[m][s];
          if (!sm.vertices) continue;
          for (int v = 0; v < sm.vertexCount; v++) {
            sm.vertices[v * 3] -= ox;
            sm.vertices[v * 3 + 1] -= oy;
            sm.vertices[v * 3 + 2] -= oz;
          }
          UpdateMeshBuffer(sm, 0, sm.vertices, sizeof(float) * sm.vertexCount * 3, 0);
        }
      }
    }

    for (int s = 0; s < L->subCount[m]; s++) {
      // Model meshes are shared (not owned); an empty slot falls back to the
      // procedural tuft.
      Mesh src = (fromModel && cfg->meshes[m][s].vertexCount > 0)
                     ? cfg->meshes[m][s] : f->defaultTuft;
      L->nearMesh[m][s] = src;
      if (L->cfg.farKeepRatio < 0.999f) {
        // Cap the far mesh to a fixed triangle budget so a heavy imported
        // model gets decimated much harder than its authored ratio would —
        // the budget is split across the variant's submeshes so the SUM stays
        // capped (the tuft, already under budget, keeps its ratio unchanged).
        L->farMesh[m][s] = BrushFoliageBuildLODMeshTarget(
            L->nearMesh[m][s], L->cfg.farKeepRatio,
            (L->cfg.tree ? BRUSH_FOLIAGE_TREE_FAR_MAX_TRIS
                         : BRUSH_FOLIAGE_FAR_MAX_TRIS) /
                L->subCount[m]);
        L->hasFar[m][s] = true;
      } else {
        L->hasFar[m][s] = false;
      }
      L->material[m][s] = LoadMaterialDefault();
      L->material[m][s].shader = f->shader;
      Texture2D albedo = (fromModel && cfg->albedos[m][s].id != 0)
                             ? cfg->albedos[m][s] : f->defaultGradient;
      L->material[m][s].maps[MATERIAL_MAP_DIFFUSE].texture = albedo;
      L->material[m][s].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    }
    // Bake the variant's billboard impostor once (opt-in): ALL submeshes
    // render into one atlas pair. Materials are set above — the bake reads
    // each submesh's albedo from them.
    if (f->impostor) BrushFoliageBakeImpostor(f, L, m);
  }
  // Billboard band covers the outer ~60% of the far range (mesh near, impostor
  // far); 0 disables it. A wider band means the impostors dissolve (height-fade)
  // over a LONGER distance, melting into the ground/speckles instead of a hard
  // bright edge. Kept short of drawDistance's fade so the swap hides. Authored
  // billboardDist (or the tree default of 90 m) overrides the derivation.
  L->billboardDistance =
      (f->impostor)
          ? ((L->cfg.billboardDist > 0.0f)
                 ? L->cfg.billboardDist
                 : (L->cfg.tree
                        ? fminf(90.0f, L->cfg.drawDistance * 0.5f)
                        : L->cfg.lodDistance +
                              (L->cfg.drawDistance - L->cfg.lodDistance) * 0.4f))
          : 0.0f;
  // Worst-case instance extent for the cull margins (cone + chunk box): a
  // 15 m canopy must not pop when its BASE leaves the view cone.
  L->cullPad = layerExt * L->cfg.scale * (1.0f + L->cfg.scaleJitter);

  // Per-model visible buffers, each sized to the layer's worst-case (one variant
  // could dominate). The cull bounds-checks, so undersizing only drops instances.
  float dd = L->cfg.drawDistance;
  long cap = (long)(L->cfg.density * (2.0f * dd) * (2.0f * dd) * 1.2f *
                    BRUSH_FOLIAGE_MAX_BOOST) + 256; // painting can thicken
  if (cap > 900000) cap = 900000;
  L->nearB.cap = L->farB.cap = (int)cap;
  L->nearB.modelCount = L->farB.modelCount = mc;
  L->bbB.cap = f->impostor ? (int)cap : 0; // no billboard buffers when off
  L->bbB.modelCount = mc;
  for (int m = 0; m < mc; m++) {
    L->nearB.buf[m] = MemAlloc(sizeof(Matrix) * (int)cap);
    L->farB.buf[m] = MemAlloc(sizeof(Matrix) * (int)cap);
    if (f->impostor) L->bbB.buf[m] = MemAlloc(sizeof(Matrix) * (int)cap);
  }
  // F3: green the terrain where this layer grows so the ground reads as a grass
  // field past the 3D blades (fills the bare horizon). Last tinted layer wins —
  // author the dominant grass layer last if several are tinted.
  if (L->cfg.groundStrength > 0.0f)
    BrushRenderSetGrassGround(L->cfg.groundColor, L->cfg.groundStrength,
                              L->cfg.growLayer, L->cfg.drawDistance);
  return f->layerCount++;
}

void BrushFoliageClearLayers(BrushFoliage *f) {
  for (int i = 0; i < f->layerCount; i++) {
    BrushFoliageLayer *L = &f->layers[i];
    for (int m = 0; m < L->meshCount; m++) {
      for (int s = 0; s < L->subCount[m]; s++) {
        if (L->hasFar[m][s]) UnloadMesh(L->farMesh[m][s]);
        // nearMesh is a shared model/tuft mesh — not owned, not freed here.
        // Shader + albedo are system/caller-owned; detach before UnloadMaterial.
        L->material[m][s].shader = (Shader){0};
        L->material[m][s].maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){0};
        UnloadMaterial(L->material[m][s]);
      }
      if (L->hasImpostor[m]) {
        UnloadMesh(L->billboardMesh[m]);
        L->billboardMat[m].shader = (Shader){0};
        L->billboardMat[m].maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){0};
        L->billboardMat[m].maps[MATERIAL_MAP_NORMAL].texture = (Texture2D){0};
        UnloadMaterial(L->billboardMat[m]);
        UnloadRenderTexture(L->impostorRT[m]);    // owns the albedo atlas
        UnloadRenderTexture(L->impostorNrmRT[m]); // owns the normal atlas
      }
      if (L->nearB.buf[m]) MemFree(L->nearB.buf[m]);
      if (L->farB.buf[m]) MemFree(L->farB.buf[m]);
      if (L->bbB.buf[m]) MemFree(L->bbB.buf[m]);
    }
    *L = (BrushFoliageLayer){0};
  }
  f->layerCount = 0;
  // Drop the grass-ground tint; a following re-add re-applies it (editor
  // re-scatter clears then re-adds). Off = no terrain tint.
  BrushRenderSetGrassGround((Vector3){0, 0, 0}, 0.0f, -1, 1.0f);
}

void BrushFoliageSetWind(BrushFoliage *f, Vector2 dir, float strength) {
  float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
  if (len > 1e-4f) { dir.x /= len; dir.y /= len; }
  f->windDir = dir;
  f->windStrength = strength;
}

// --- Scatter (worker thread) ------------------------------------------------

static unsigned int HashCell(int x, int z, unsigned int seed) {
  unsigned int h = seed ^ ((unsigned)x * 73856093u) ^ ((unsigned)z * 19349663u);
  h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
  return h;
}

typedef struct {
  const BrushChunkSamplers *s; // height + density + splat samplers (chunk-local)
  const BrushFoliageLayerConfig *cfg;
  unsigned int seed;  // per-LAYER (constant across chunks) -> seamless placement
  int paintLayer;     // foliage density-mask channel (0..3, or -1 = no mask)
  int meshCount;      // model variants -> per-instance modelIdx (>=1)
} ScatterCtx;

// True if this position passes the layer's surface-layer rules (grow/avoid).
// `r` = the instance's scaled footprint radius: the road exclusion must hold
// for the whole CLUMP, not just its centre point — a wide model placed on the
// shoulder used to pass the centre test and hang its canopy over the road.
static bool SurfacePasses(const ScatterCtx *s, float x, float z, float r) {
  // Roads are a separate coverage mask (not a splat slot), so exclude foliage
  // from them here rather than via avoidLayer.
  if (s->cfg->avoidRoad && s->s->roadAt) {
    if (s->s->roadAt(s->s->ctx, x, z) > 0.35f) return false;
    if (r > 0.6f) { // wide clump: probe the footprint's compass points too
      float pr = r * 0.8f;
      if (s->s->roadAt(s->s->ctx, x + pr, z) > 0.5f ||
          s->s->roadAt(s->s->ctx, x - pr, z) > 0.5f ||
          s->s->roadAt(s->s->ctx, x, z + pr) > 0.5f ||
          s->s->roadAt(s->s->ctx, x, z - pr) > 0.5f)
        return false;
    }
  }
  if (!s->s->splatAt || (s->cfg->growLayer < 0 && s->cfg->avoidLayer < 0))
    return true;
  float w[4];
  s->s->splatAt(s->s->ctx, x, z, w);
  if (s->cfg->avoidLayer >= 0 && s->cfg->avoidLayer < 4) {
    float thr = s->cfg->avoidThreshold > 0.0f ? s->cfg->avoidThreshold : 0.5f;
    if (w[s->cfg->avoidLayer] > thr) return false; // e.g. on the road layer
  }
  if (s->cfg->growLayer >= 0 && s->cfg->growLayer < 4) {
    int dom = 0;
    for (int L = 1; L < 4; L++) if (w[L] > w[dom]) dom = L;
    if (dom != s->cfg->growLayer) return false; // wrong dominant terrain layer
  }
  return true;
}

// Rotation that tilts local up (0,1,0) onto the terrain normal, so a clump's
// grounded base lies FLUSH with a slope instead of floating on the downhill
// side. `blend` (0..1) eases toward the true normal — 1 = full slope-align
// (donor behaviour), lower keeps tall grass more upright. Matches the donor's
// WorldNormal + MatrixAlignUp (racer src/world.c).
static Matrix FoliageAlignUp(float nx, float ny, float nz, float blend) {
  // Blend the surface normal toward straight-up, then renormalise.
  nx *= blend; nz *= blend; ny = 1.0f + (ny - 1.0f) * blend;
  float len = sqrtf(nx * nx + ny * ny + nz * nz);
  if (len < 1e-6f) return MatrixIdentity();
  nx /= len; ny /= len; nz /= len;
  if (ny > 0.9999f) return MatrixIdentity();
  if (ny < -0.9999f) return MatrixRotateX(3.14159265f);
  Vector3 axis = Vector3Normalize((Vector3){nz, 0.0f, -nx}); // cross(up, normal)
  return MatrixRotate(axis, acosf(ny));
}

static void FoliagePlace(void *ud, BrushFoliageSet *set, int *index, int maxCount,
                         int gx, int gz, float bx, float bz, float cw, float cd,
                         float cy) {
  (void)gx; (void)gz; (void)cy;
  ScatterCtx *s = (ScatterCtx *)ud;
  if (*index >= maxCount) return;

  // Hash the WORLD cell (not the local grid index) so borders match: adjacent
  // chunks' grids tile, and a given world cell hashes identically everywhere.
  int wcx = (int)lroundf(bx / cw);
  int wcz = (int)lroundf(bz / cd);
  unsigned int h0 = HashCell(wcx, wcz, s->seed);

  // Painted density multiplier -> instances this cell emits (multi-emit boost;
  // the grid stays fixed-resolution so borders stay seamless).
  float M = s->s->densityAt ? s->s->densityAt(s->s->ctx, bx, bz, s->paintLayer)
                            : 1.0f;
  if (M <= 0.0f) return;
  int n = (int)M;
  if ((float)(h0 & 0xffff) / 65535.0f < (M - (float)n)) n++;

  for (int k = 0; k < n && *index < maxCount; k++) {
    unsigned int h = HashCell(wcx * 131 + k * 977 + 7, wcz, s->seed);
    float r1 = (float)(h & 0xffff) / 65535.0f;
    float r2 = (float)((h >> 16) & 0xffff) / 65535.0f;
    float jx = bx + (r1 - 0.5f) * cw;
    float jz = bz + (r2 - 0.5f) * cd;

    // Variant + scale FIRST: the surface tests and grounding below are
    // footprint-aware, and the footprint depends on which variant landed here.
    // (Same hash bits as before — placement stays deterministic.)
    unsigned char mi = (s->meshCount > 1)
                           ? (unsigned char)((h >> 9) % (unsigned)s->meshCount) : 0;
    float vscale = (s->cfg->meshScale[mi] > 0.0f) ? s->cfg->meshScale[mi] : 1.0f;
    float sc = s->cfg->scale * vscale *
               (1.0f + (r2 - 0.5f) * 2.0f * s->cfg->scaleJitter);
    float rEff = s->cfg->baseRadius[mi] * sc; // scaled footprint radius (m)

    if (!SurfacePasses(s, jx, jz, rEff)) continue;

    // DECISION height: LOD-independent (the fine heightmap). Every accept/
    // reject below must key on this — the mesh-matched heightAt CHANGES when
    // the chunk crosses an LOD ring and rebakes, and any decision keyed on it
    // makes instances appear/vanish at ring boundaries ("foliage respawns at
    // mid distance", trees popping from thin air on approach).
    float (*fineAt)(void *, float, float) =
        s->s->heightFineAt ? s->s->heightFineAt : s->s->heightAt;
    float yd = fineAt(s->s->ctx, jx, jz);
    if (s->cfg->minHeight < s->cfg->maxHeight) {
      if (yd < s->cfg->minHeight || yd > s->cfg->maxHeight) continue;
      // Thin out over a fade band inside each limit rather than a hard ring:
      // grass fades to nothing AT the limit, so a blade grounded a few cm under
      // maxHeight (mesh surface sits below the 1 m heightmap on broad peaks)
      // doesn't survive as a stray band right at the ceiling.
      float band = fminf(2.0f, (s->cfg->maxHeight - s->cfg->minHeight) * 0.4f);
      if (band > 0.001f) {
        float edge = fminf(yd - s->cfg->minHeight, s->cfg->maxHeight - yd);
        float keep = edge / band; // 0 at a limit -> 1 a full band inside
        if (keep < 1.0f) {
          float rp = (float)((h >> 7) & 0xffff) / 65535.0f;
          if (rp > keep) continue;
        }
      }
    }
    
    // Central-difference terrain normal (also drives the slope reject) — from
    // the FINE height so the slope decision is LOD-stable; the tilt it drives
    // is a look, not a grounding, so fine-vs-mesh error is invisible.
    float e = 0.5f;
    float hx = fineAt(s->s->ctx, jx + e, jz) - fineAt(s->s->ctx, jx - e, jz);
    float hz = fineAt(s->s->ctx, jx, jz + e) - fineAt(s->s->ctx, jx, jz - e);
    float nlen = sqrtf(hx * hx + hz * hz + 4.0f * e * e);
    float nx = -hx / nlen, ny = (2.0f * e) / nlen, nz = -hz / nlen;
    if (s->cfg->maxSlopeDeg > 0.0f &&
        acosf(ny) * 57.29578f > s->cfg->maxSlopeDeg) continue; // too steep

    // Biome gate: grow only in this layer's biome, thinning to nothing across
    // the border (the neighbour biome's layers fade in over the same band, so
    // total density is conserved). Probabilistic reject on a distinct hash slice.
    if (s->cfg->biomeId >= 0 && s->s->biomeAt) {
      BrushBiomeSample bs;
      s->s->biomeAt(s->s->ctx, jx, jz, &bs);
      float bw = (bs.id0 == s->cfg->biomeId) ? (1.0f - bs.blend) : 0.0f;
      if (bs.id1 == s->cfg->biomeId) bw += bs.blend;
      if (bw <= 0.001f) continue;
      if (bw < 1.0f) {
        float rp = (float)((h >> 3) & 0xffff) / 65535.0f;
        if (rp > bw) continue;
      }
    }

    // GROUNDING height: trees use the FINE height (fully LOD-independent, so
    // a tree never snaps when its chunk crosses an LOD ring — the sub-metre
    // fine-vs-coarse-mesh error is invisible under a 10 m tree at 250 m, and
    // racer grounded its always-visible trees the same way). Grass grounds to
    // the RENDERED mesh: blades sit exactly on the surface, and when the mesh
    // refines their Y moves WITH the terrain, which the eye forgives.
    float y = s->cfg->tree ? yd : s->s->heightAt(s->s->ctx, jx, jz);
    float (*groundAt)(void *, float, float) =
        s->cfg->tree ? fineAt : s->s->heightAt;

    // Wide clumps: ground to the LOWEST of four footprint probes (capped) so a
    // big model on convex or steep ground can't hang its downhill edge in the
    // air — the centre tap alone leaves r-wide meshes floating on hills. Small
    // grass (r <= 0.6 m) keeps the single-tap fast path. The slope tilt below
    // still conforms the base; this fixes what the tilt's 0.75 blend leaves.
    if (rEff > 0.6f) {
      float pr = rEff * 0.7f;
      float ymin = fminf(fminf(groundAt(s->s->ctx, jx + pr, jz),
                               groundAt(s->s->ctx, jx - pr, jz)),
                         fminf(groundAt(s->s->ctx, jx, jz + pr),
                               groundAt(s->s->ctx, jx, jz - pr)));
      if (ymin < y) y = fmaxf(ymin, y - rEff * 0.8f); // cap the sink
    }

    float yaw = r1 * 6.2831853f;
    // The mesh base is baked to Y=0 at layer setup (AddLayer grounds + centres
    // the geometry), so the scatter point maps straight to the terrain height.
    int i = *index;
    set->positions[i] = (Vector3){jx, y, jz};
    // scale -> yaw -> tilt-to-slope -> translate. The tilt rotates about the
    // (grounded) base, so the base point stays on the terrain and the footprint
    // conforms to the slope. 0.75 blend keeps grass from lying fully flat on
    // extreme cliffs while still killing the float.
    Matrix align = FoliageAlignUp(nx, ny, nz, 0.75f);
    set->transforms[i] = MatrixMultiply(
        MatrixMultiply(MatrixMultiply(MatrixScale(sc, sc, sc), MatrixRotateY(yaw)), align),
        MatrixTranslate(jx, y + s->cfg->heightOffset, jz));
    set->modelIdx[i] = mi;
    (*index)++;
  }
}

void *BrushFoliageChunkBake(void *user, BrushChunkCoord coord, Vector3 origin,
                            float size, const BrushChunkSamplers *samplers) {
  (void)coord;
  BrushFoliage *f = (BrushFoliage *)user;
  FoliageChunkHandle *H = MemAlloc(sizeof(FoliageChunkHandle));
  memset(H, 0, sizeof(*H));
  H->setCount = f->layerCount;
  Vector3 center = {origin.x + size * 0.5f, 0.0f, origin.z + size * 0.5f};
  for (int li = 0; li < f->layerCount; li++) {
    BrushFoliageLayer *L = &f->layers[li];
    // Grid at base density; painting boosts up to MAX_BOOST via multi-emit, so
    // size the cap for the ceiling.
    long cap = (long)(L->cfg.density * size * size * BRUSH_FOLIAGE_MAX_BOOST) + 8;
    if (cap > 300000) cap = 300000;
    ScatterCtx pc = {samplers, &L->cfg,
                     0x9e3779b9u + (unsigned)li * 2246822519u,
                     (li < BRUSH_FOLIAGE_PAINT_MAX) ? li : -1, L->meshCount};
    BrushFoliageScatterGrid(&H->sets[li], center, size, size, L->cfg.density,
                            BRUSH_FOLIAGE_MAX_BOOST, (int)cap, FoliagePlace, &pc);
    BrushFoliageBuildGrid(&H->sets[li], center, size, size, 4.0f);
  }
  return H;
}

void BrushFoliageChunkFree(void *user, void *handle) {
  (void)user;
  if (!handle) return;
  FoliageChunkHandle *H = (FoliageChunkHandle *)handle;
  for (int i = 0; i < H->setCount; i++) BrushFoliageSetCleanup(&H->sets[i]);
  MemFree(H);
}

// --- Draw (main thread, inside the render scene callback) -------------------

static void FoliageSceneCb(void *user, Camera3D cam) {
  BrushFoliage *f = (BrushFoliage *)user;
  if (!f->world || f->layerCount == 0) return;

  static BrushWorldChunkView views[2048];
  int nv = BrushWorldGetActiveChunks(f->world, views, 2048);
  if (nv == 0) return;

  BrushFrustum frustum = BrushRenderMakeFrustum(cam);

  rlDisableBackfaceCulling(); // grass cards are two-sided
  for (int li = 0; li < f->layerCount; li++) {
    BrushFoliageLayer *L = &f->layers[li];
    float ds = f->distanceScale * f->qualityScale; // zoom * quality preset
    // Trees keep their full draw distance on every quality preset: the outer
    // tier is 2-tri billboards (cheap), and a preset that makes TREES vanish
    // at 175 m reads as a bug, not a setting (racer never distance-culled
    // trees at all). Quality still scales the expensive mesh tiers via lod/
    // billboard distances below.
    float draw = L->cfg.drawDistance * (L->cfg.tree ? f->distanceScale : ds);
    float lod = L->cfg.lodDistance * ds;

    float bbDist = (L->billboardDistance > 0.0f) ? L->billboardDistance * ds : 0.0f;

    // Reset the per-model batch counts, then gather every chunk into them.
    for (int m = 0; m < L->meshCount; m++) {
      L->nearB.count[m] = 0; L->farB.count[m] = 0; L->bbB.count[m] = 0;
    }
    for (int ci = 0; ci < nv; ci++) {
      FoliageChunkHandle *H = (FoliageChunkHandle *)views[ci].handle;
      if (li >= H->setCount) continue;

      // Distance culling: closest point on chunk's XZ AABB to camera
      float camX = cam.position.x;
      float camZ = cam.position.z;
      float minX = views[ci].origin.x;
      float minZ = views[ci].origin.z;
      float maxX = minX + views[ci].size;
      float maxZ = minZ + views[ci].size;
      float dx = (camX < minX) ? (minX - camX) : ((camX > maxX) ? (camX - maxX) : 0.0f);
      float dz = (camZ < minZ) ? (minZ - camZ) : ((camZ > maxZ) ? (camZ - maxZ) : 0.0f);
      if (dx * dx + dz * dz > draw * draw) continue;

      // Frustum culling: chunk AABB padded by the layer's worst-case instance
      // extent (union mesh bounds x scale) so tall/wide models never pop when
      // their root chunk leaves the frustum.
      float pad = L->cullPad;
      if (pad < 5.0f) pad = 5.0f;
      BoundingBox chunkBox = {
        .min = { minX - pad, -50.0f, minZ - pad },
        .max = { maxX + pad, views[ci].maxY + pad, maxZ + pad }
      };
      if (!BrushFrustumContainsBox(&frustum, chunkBox)) continue;

      BrushFoliageCull(&H->sets[li], cam.position, cam.target, draw, lod,
                       bbDist, L->cullPad, !L->cfg.tree, &L->nearB, &L->farB,
                       &L->bbB);
    }
    int nc = 0, fc = 0, bc = 0;
    for (int m = 0; m < L->meshCount; m++) {
      nc += L->nearB.count[m]; fc += L->farB.count[m]; bc += L->bbB.count[m];
    }
    L->lastNear = nc;
    L->lastFar = fc + bc;
    // BRUSH_FOLIAGE_DEBUG=1: once a second, the tier counts + distances that
    // decide everything — the ground truth for "trees pop in" reports.
    if (getenv("BRUSH_FOLIAGE_DEBUG")) {
      static double lastLog = 0.0;
      if (li == 0) { /* reset gate once per frame set */ }
      double now = GetTime();
      if (now - lastLog > 1.0) {
        if (li == f->layerCount - 1) lastLog = now;
        int resident = 0; // pre-cull instances across all active chunk sets
        for (int ci = 0; ci < nv; ci++) {
          FoliageChunkHandle *H = (FoliageChunkHandle *)views[ci].handle;
          if (li < H->setCount) resident += H->sets[li].count;
        }
        TraceLog(LOG_INFO,
                 "FOLIAGE L%d tree=%d near=%d far=%d bb=%d resident=%d "
                 "chunks=%d imp=%d draw=%.0f lod=%.0f bbDist=%.0f q=%.2f "
                 "cam=(%.0f,%.0f,%.0f)->(%.0f,%.0f)",
                 li, L->cfg.tree, nc, fc, bc, resident, nv,
                 (int)L->hasImpostor[0], draw, lod, bbDist, f->qualityScale,
                 cam.position.x, cam.position.y, cam.position.z,
                 cam.target.x, cam.target.z);
      }
    }
    if (nc == 0 && fc == 0 && bc == 0) continue;

    float wind = f->windStrength * L->cfg.windStrength;
    SetShaderValue(f->shader, f->locTime, &f->time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(f->shader, f->locWindDir, &f->windDir, SHADER_UNIFORM_VEC2);
    SetShaderValue(f->shader, f->locWindStr, &wind, SHADER_UNIFORM_FLOAT);
    float cutoff = 0.3f;
    SetShaderValue(f->shader, f->locMacroLow, &L->cfg.macroLow, SHADER_UNIFORM_VEC3);
    SetShaderValue(f->shader, f->locMacroHigh, &L->cfg.macroHigh, SHADER_UNIFORM_VEC3);
    SetShaderValue(f->shader, f->locGrassTint, &L->cfg.tint, SHADER_UNIFORM_VEC3);
    SetShaderValue(f->shader, f->locAlphaCutoff, &cutoff, SHADER_UNIFORM_FLOAT);
    BrushRenderApplySceneLighting(f->shader);
    if (!f->qualityShadows) { // Low: skip the CSM taps in the grass shader
      float off = 0.0f;
      SetShaderValue(f->shader, GetShaderLocation(f->shader, "uShadowEnabled"),
                     &off, SHADER_UNIFORM_FLOAT);
    }

    // Transition widths MUST match BrushFoliageCull's overlap bands (shared
    // helpers) so each band's height fade-out and the next band's fade-in line
    // up exactly with the double-draw region — a clean 0<->100% cross-fade.
    float tNear = BrushFoliageNearTransition(lod);
    float tFar = BrushFoliageFarTransition(draw);
    float bbEdge = (bbDist > 0.0f) ? bbDist : draw; // far band's far edge

    // Per-band height-fade window: fade IN over [nearStart, nearEnd] (grow from
    // the ground as the previous band shrinks out) and fade OUT over
    // [fadeStart, fadeEnd]. nearEnd == 0 disables the fade-in (the near band).
    #define BRUSH_FOLIAGE_SET_FADE(nStart, nEnd, oStart, oEnd) do { \
      float _ns=(nStart), _ne=(nEnd), _os=(oStart), _oe=(oEnd); \
      SetShaderValue(f->shader, f->locFadeNearStart, &_ns, SHADER_UNIFORM_FLOAT); \
      SetShaderValue(f->shader, f->locFadeNearEnd, &_ne, SHADER_UNIFORM_FLOAT); \
      SetShaderValue(f->shader, f->locFadeStart, &_os, SHADER_UNIFORM_FLOAT); \
      SetShaderValue(f->shader, f->locFadeEnd, &_oe, SHADER_UNIFORM_FLOAT); \
    } while (0)

    // Lit mesh bands first (near + far LOD), one instanced draw per variant.
    if (f->impostor) {
      float zero = 0.0f;
      SetShaderValue(f->shader, f->locImpostor, &zero, SHADER_UNIFORM_FLOAT);
    }

    // Draw Near Mesh: no fade-in, fade OUT over the SECOND half of the overlap
    // [lod - tNear/2, lod]. Offsetting so the near mesh only starts shrinking
    // AFTER the far mesh has reached full height (see below) keeps the visible
    // height ~constant across the crossfade — otherwise both copies sit at ~50%
    // height in the middle of the band and the grass reads as lying flat, then
    // "standing up" as you approach.
    {
      BRUSH_FOLIAGE_SET_FADE(0.0f, 0.0f, lod - tNear * 0.5f, lod);
      for (int m = 0; m < L->meshCount; m++) {
        if (L->nearB.count[m] > 0)
          for (int s = 0; s < L->subCount[m]; s++) // submeshes share the batch
            DrawMeshInstanced(L->nearMesh[m][s], L->material[m][s],
                              L->nearB.buf[m], L->nearB.count[m]);
      }
    }

    // Draw Far Mesh: fade IN over the FIRST half of the overlap [lod - tNear,
    // lod - tNear/2] (full by the midpoint, before the near mesh fades), fade
    // OUT over [bbEdge - tFar, bbEdge]. The detail swap happens at the midpoint
    // where BOTH are full height, so it's a subtle mesh change, not a height dip.
    {
      BRUSH_FOLIAGE_SET_FADE(lod - tNear, lod - tNear * 0.5f, bbEdge - tFar, bbEdge);
      for (int m = 0; m < L->meshCount; m++) {
        if (L->farB.count[m] > 0)
          for (int s = 0; s < L->subCount[m]; s++)
            DrawMeshInstanced(L->hasFar[m][s] ? L->farMesh[m][s]
                                              : L->nearMesh[m][s],
                              L->material[m][s], L->farB.buf[m],
                              L->farB.count[m]);
      }
    }

    // Billboard band: fade IN over [bbDist - tFar, bbDist], then DISSOLVE across
    // the WHOLE outer band [bbDist, draw] — flat impostor cards light up bright
    // and uniform at a grazing sun, so a long height fade-out (not just the last
    // tFar) is what makes the far tier melt into the ground instead of snapping
    // off as a bright band. Peaks at bbDist, gone by draw.
    if (bc > 0) {
      float one = 1.0f;
      SetShaderValue(f->shader, f->locImpostor, &one, SHADER_UNIFORM_FLOAT);
      // Grass dissolves across the WHOLE outer band (impostor cards melt into
      // the terrain). TREES must not: a half-dissolved tree at 200 m reads as
      // "trees don't exist at distance", and approaching one plays the grow
      // backwards ("mushroom" spawn). Tree billboards hold full height and
      // only dissolve over the final edge before the cull.
      float bbFadeStart =
          L->cfg.tree ? fmaxf(draw - tFar * 2.0f, bbDist) : bbDist;
      BRUSH_FOLIAGE_SET_FADE(bbDist - tFar, bbDist, bbFadeStart, draw);
      for (int m = 0; m < L->meshCount; m++) {
        if (L->bbB.count[m] > 0 && L->hasImpostor[m])
          DrawMeshInstanced(L->billboardMesh[m], L->billboardMat[m],
                            L->bbB.buf[m], L->bbB.count[m]);
      }
      float zero = 0.0f;
      SetShaderValue(f->shader, f->locImpostor, &zero, SHADER_UNIFORM_FLOAT);
    }
    #undef BRUSH_FOLIAGE_SET_FADE
  }
  rlEnableBackfaceCulling();
}

void BrushFoliageInstallHooks(BrushFoliage *f, BrushWorldConfig *cfg) {
  cfg->chunkBake = BrushFoliageChunkBake;
  cfg->chunkFree = BrushFoliageChunkFree;
  cfg->chunkUser = f;
}

void BrushFoliageAttach(BrushFoliage *f, BrushWorld *world) {
  f->world = world;
  BrushRenderSetSceneCallback(FoliageSceneCb, f);
}

void BrushFoliageUpdate(BrushFoliage *f, float time, float distanceScale) {
  f->time = time;
  f->distanceScale = (distanceScale > 0.0f) ? distanceScale : 1.0f;
}

void BrushFoliageSetQuality(BrushFoliage *f, BrushFoliageQuality q) {
  f->quality = q;
  switch (q) {
    case BRUSH_FOLIAGE_LOW:  f->qualityScale = 0.5f;  f->qualityShadows = false; break;
    case BRUSH_FOLIAGE_MED:  f->qualityScale = 0.75f; f->qualityShadows = true;  break;
    default:                 f->qualityScale = 1.0f;  f->qualityShadows = true;  break;
  }
}

BrushFoliageQuality BrushFoliageGetQuality(const BrushFoliage *f) {
  return f ? f->quality : BRUSH_FOLIAGE_HIGH;
}

int BrushFoliageLayerCount(const BrushFoliage *f) { return f ? f->layerCount : 0; }

void BrushFoliageLayerStats(const BrushFoliage *f, int layer, int *nearCount,
                            int *farCount) {
  int nc = 0, fc = 0;
  if (f && layer >= 0 && layer < f->layerCount) {
    nc = f->layers[layer].lastNear;
    fc = f->layers[layer].lastFar;
  }
  if (nearCount) *nearCount = nc;
  if (farCount) *farCount = fc;
}

void BrushFoliageDestroy(BrushFoliage *f) {
  if (!f) return;
  if (f->world) BrushRenderSetSceneCallback(NULL, NULL);
  BrushFoliageClearLayers(f);
  UnloadMesh(f->defaultTuft);
  UnloadTexture(f->defaultGradient);
  UnloadShader(f->shader);
  MemFree(f);
}
