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

// Per-instance visibility: distance^2 (out) + a GENEROUS behind-the-camera cull.
// The forward vector is XZ-projected, which is unreliable when the camera looks
// down (third-person, inspecting), so a tight FOV cone wrongly culls on-screen
// side/near grass and it pops as you move. Keep everything except grass clearly
// behind the camera (>12 m back along the view). GPU frustum culling handles the
// rest at raster; a few extra off-screen instances in the vertex pass are cheap.
static inline bool InstanceVisible(const BrushFoliageSet *set, int i,
                                   Vector3 viewPos, float fwdX, float fwdZ,
                                   float maxDist2, float *dist2Out) {
  float dx = set->positions[i].x - viewPos.x;
  float dz = set->positions[i].z - viewPos.z;
  float dist2 = dx * dx + dz * dz;
  if (dist2 > maxDist2) return false;
  if (dist2 >= 36.0f && (dx * fwdX + dz * fwdZ) < -12.0f) return false; // far behind
  *dist2Out = dist2;
  return true;
}

// Append a transform to the batch buffer for its model variant (bounds-checked).
static inline void BatchAppend(BrushFoliageDrawBatch *b, unsigned char mi,
                               Matrix m) {
  int idx = (mi < b->modelCount) ? mi : 0;
  if (b->count[idx] < b->cap) b->buf[idx][b->count[idx]++] = m;
}

void BrushFoliageCull(const BrushFoliageSet *set, Vector3 viewPos,
                      Vector3 viewTarget, float drawDistance, float lodDistance,
                      BrushFoliageDrawBatch *nearB, BrushFoliageDrawBatch *farB) {
  CullWalk w;
  if (!CullWalkBegin(set, viewPos, viewTarget, drawDistance, &w)) return;

  float maxDist2 = drawDistance * drawDistance;
  float lodDist2 = lodDistance * lodDistance;

  for (int cz = w.startCz; cz != w.endCz + w.stepZ; cz += w.stepZ) {
    for (int cx = w.startCx; cx != w.endCx + w.stepX; cx += w.stepX) {
      BrushFoliageGridCell *cell = &set->cells[cz * set->gridResX + cx];
      if (cell->count == 0) continue;

      // Skip the whole cell if its bounding sphere is behind the camera.
      float cwx = set->originX + (cx + 0.5f) * set->cellSize;
      float cwz = set->originZ + (cz + 0.5f) * set->cellSize;
      float projC = (cwx - viewPos.x) * w.fwdX + (cwz - viewPos.z) * w.fwdZ;
      if (projC < -(w.cellRadiusWorld + 12.0f)) continue;

      for (int k = 0; k < cell->count; k++) {
        int i = set->gridIndices[cell->offset + k];
        float dist2;
        if (!InstanceVisible(set, i, viewPos, w.fwdX, w.fwdZ, maxDist2, &dist2))
          continue;
        unsigned char mi = set->modelIdx ? set->modelIdx[i] : 0;
        BatchAppend(dist2 < lodDist2 ? nearB : farB, mi, set->transforms[i]);
      }
    }
  }
}

void BrushFoliageCull3(const BrushFoliageSet *set, Vector3 viewPos,
                       Vector3 viewTarget, float drawDistance, float lodDistance,
                       float billboardDistance,
                       Matrix *outNear, int maxNear, int *nearCount,
                       Matrix *outFar, int maxFar, int *farCount,
                       Matrix *outBillboard, int maxBillboard, int *billboardCount) {
  CullWalk w;
  if (!CullWalkBegin(set, viewPos, viewTarget, drawDistance, &w)) return;

  float maxDist2 = drawDistance * drawDistance;
  float lodDist2 = lodDistance * lodDistance;
  float bbDist2 = billboardDistance * billboardDistance;
  int nc = *nearCount, fc = *farCount, bc = *billboardCount;

  for (int cz = w.startCz; cz != w.endCz + w.stepZ; cz += w.stepZ) {
    for (int cx = w.startCx; cx != w.endCx + w.stepX; cx += w.stepX) {
      BrushFoliageGridCell *cell = &set->cells[cz * set->gridResX + cx];
      if (cell->count == 0) continue;

      float cwx = set->originX + (cx + 0.5f) * set->cellSize;
      float cwz = set->originZ + (cz + 0.5f) * set->cellSize;
      float projC = (cwx - viewPos.x) * w.fwdX + (cwz - viewPos.z) * w.fwdZ;
      if (projC < -(w.cellRadiusWorld + 12.0f)) continue;

      for (int k = 0; k < cell->count; k++) {
        int i = set->gridIndices[cell->offset + k];
        float dist2;
        if (!InstanceVisible(set, i, viewPos, w.fwdX, w.fwdZ, maxDist2, &dist2))
          continue;
        if (dist2 < lodDist2) {
          if (nc < maxNear) outNear[nc++] = set->transforms[i];
        } else if (dist2 < bbDist2) {
          if (fc < maxFar) outFar[fc++] = set->transforms[i];
        } else {
          if (bc < maxBillboard) outBillboard[bc++] = set->transforms[i];
        }
      }
    }
  }

  *nearCount = nc;
  *farCount = fc;
  *billboardCount = bc;
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
  Mesh billboard = {0};

  // Bounding box of the source mesh.
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
  return s;
}

// --- Foliage system (layers streamed over the chunk world) ------------------

typedef struct BrushFoliageLayer {
  BrushFoliageLayerConfig cfg;
  int meshCount;   // model variants (>=1; a meadow mixes several meshes)
  Mesh nearMesh[BRUSH_FOLIAGE_MODELS_PER_LAYER]; // wind-baked copy (models) or tuft
  Mesh farMesh[BRUSH_FOLIAGE_MODELS_PER_LAYER];  // decimated LOD, owned
  bool hasFar[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  Material material[BRUSH_FOLIAGE_MODELS_PER_LAYER];
  BrushFoliageDrawBatch nearB, farB; // per-model visible buffers (main thread)
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
  int locTime, locWindDir, locWindStr, locFadeStart, locFadeEnd, locFadeNearEnd;
  int locMacroLow, locMacroHigh, locGrassTint, locAlphaCutoff;
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
  f->defaultTuft = BrushFoliageMakeGrassPatch(120, 0.45f, 0.24f);
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
  f->locFadeNearEnd = GetShaderLocation(f->shader, "uFadeNearEnd");
  f->locMacroLow = GetShaderLocation(f->shader, "uMacroLow");
  f->locMacroHigh = GetShaderLocation(f->shader, "uMacroHigh");
  f->locGrassTint = GetShaderLocation(f->shader, "uGrassTint");
  f->locAlphaCutoff = GetShaderLocation(f->shader, "uAlphaCutoff");
  return f;
}

int BrushFoliageAddLayer(BrushFoliage *f, const BrushFoliageLayerConfig *cfg) {
  if (f->layerCount >= BRUSH_FOLIAGE_MAX_LAYERS) return -1;
  BrushFoliageLayer *L = &f->layers[f->layerCount];
  L->cfg = *cfg;
  if (L->cfg.density <= 0.0f) L->cfg.density = 1.0f;
  if (L->cfg.drawDistance <= 0.0f) L->cfg.drawDistance = 80.0f;
  if (L->cfg.lodDistance <= 0.0f) L->cfg.lodDistance = L->cfg.drawDistance * 0.4f;
  if (L->cfg.scale <= 0.0f) L->cfg.scale = 1.0f;
  if (L->cfg.farKeepRatio <= 0.0f) L->cfg.farKeepRatio = 0.4f;
  if (L->cfg.macroLow.x == 0 && L->cfg.macroLow.y == 0 && L->cfg.macroLow.z == 0)
    L->cfg.macroLow = (Vector3){0.75f, 0.82f, 0.55f};
  if (L->cfg.macroHigh.x == 0 && L->cfg.macroHigh.y == 0 && L->cfg.macroHigh.z == 0)
    L->cfg.macroHigh = (Vector3){1.05f, 1.05f, 0.78f};
  if (L->cfg.tint.x == 0 && L->cfg.tint.y == 0 && L->cfg.tint.z == 0)
    L->cfg.tint = (Vector3){1, 1, 1};

  // Model palette: build near + far LOD mesh and a material per variant. An
  // empty palette (or a variant with no mesh) falls back to the procedural tuft;
  // a variant with no albedo falls back to the gradient.
  int mc = cfg->meshCount;
  if (mc <= 0) mc = 1;
  if (mc > BRUSH_FOLIAGE_MODELS_PER_LAYER) mc = BRUSH_FOLIAGE_MODELS_PER_LAYER;
  L->meshCount = mc;
  for (int m = 0; m < mc; m++) {
    // The wind bends by world height, so a model mesh works as-is (shared,
    // not owned); an empty slot falls back to the procedural tuft.
    L->nearMesh[m] = (cfg->meshCount > 0 && cfg->meshes[m].vertexCount > 0)
                         ? cfg->meshes[m] : f->defaultTuft;
    // Lowest vertex in model space -> anchor the base to the terrain at scatter
    // time (models often pivot at their centre, so they'd float/sink otherwise).
    float minY = 0.0f;
    Mesh nm = L->nearMesh[m];
    if (nm.vertices && nm.vertexCount > 0) {
      minY = nm.vertices[1];
      for (int v = 1; v < nm.vertexCount; v++)
        if (nm.vertices[v * 3 + 1] < minY) minY = nm.vertices[v * 3 + 1];
    }
    L->cfg.meshBaseY[m] = minY;
    if (L->cfg.farKeepRatio < 0.999f) {
      L->farMesh[m] = BrushFoliageBuildLODMesh(L->nearMesh[m], L->cfg.farKeepRatio);
      L->hasFar[m] = true;
    } else {
      L->hasFar[m] = false;
    }
    L->material[m] = LoadMaterialDefault();
    L->material[m].shader = f->shader;
    L->material[m].maps[MATERIAL_MAP_DIFFUSE].texture =
        (cfg->meshCount > 0 && cfg->albedos[m].id != 0) ? cfg->albedos[m]
                                                        : f->defaultGradient;
    L->material[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  }

  // Per-model visible buffers, each sized to the layer's worst-case (one variant
  // could dominate). The cull bounds-checks, so undersizing only drops instances.
  float dd = L->cfg.drawDistance;
  long cap = (long)(L->cfg.density * (2.0f * dd) * (2.0f * dd) * 1.2f *
                    BRUSH_FOLIAGE_MAX_BOOST) + 256; // painting can thicken
  if (cap > 900000) cap = 900000;
  L->nearB.cap = L->farB.cap = (int)cap;
  L->nearB.modelCount = L->farB.modelCount = mc;
  for (int m = 0; m < mc; m++) {
    L->nearB.buf[m] = MemAlloc(sizeof(Matrix) * (int)cap);
    L->farB.buf[m] = MemAlloc(sizeof(Matrix) * (int)cap);
  }
  return f->layerCount++;
}

void BrushFoliageClearLayers(BrushFoliage *f) {
  for (int i = 0; i < f->layerCount; i++) {
    BrushFoliageLayer *L = &f->layers[i];
    for (int m = 0; m < L->meshCount; m++) {
      if (L->hasFar[m]) UnloadMesh(L->farMesh[m]);
      // nearMesh is a shared model/tuft mesh — not owned, not freed here.
      // Shader + albedo are system/caller-owned; detach before UnloadMaterial.
      L->material[m].shader = (Shader){0};
      L->material[m].maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){0};
      UnloadMaterial(L->material[m]);
      if (L->nearB.buf[m]) MemFree(L->nearB.buf[m]);
      if (L->farB.buf[m]) MemFree(L->farB.buf[m]);
    }
    *L = (BrushFoliageLayer){0};
  }
  f->layerCount = 0;
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
static bool SurfacePasses(const ScatterCtx *s, float x, float z) {
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

    if (!SurfacePasses(s, jx, jz)) continue;

    float y = s->s->heightAt(s->s->ctx, jx, jz);
    if (s->cfg->maxSlopeDeg > 0.0f) {
      float e = 0.5f;
      float hx = s->s->heightAt(s->s->ctx, jx + e, jz) - s->s->heightAt(s->s->ctx, jx - e, jz);
      float hz = s->s->heightAt(s->s->ctx, jx, jz + e) - s->s->heightAt(s->s->ctx, jx, jz - e);
      float ny = (2.0f * e) / sqrtf(hx * hx + hz * hz + 4.0f * e * e);
      if (acosf(ny) * 57.29578f > s->cfg->maxSlopeDeg) continue; // too steep
    }

    // Random model variant (deterministic per instance), then scale by that
    // variant's own factor so mixed meshes (rock + grass) size independently.
    unsigned char mi = (s->meshCount > 1)
                           ? (unsigned char)((h >> 9) % (unsigned)s->meshCount) : 0;
    float vscale = (s->cfg->meshScale[mi] > 0.0f) ? s->cfg->meshScale[mi] : 1.0f;
    float yaw = r1 * 6.2831853f;
    float sc = s->cfg->scale * vscale *
               (1.0f + (r2 - 0.5f) * 2.0f * s->cfg->scaleJitter);
    // Anchor the model's BASE (its lowest vertex, scaled) to the terrain, so
    // centre-pivoted models don't float or sink.
    float baseY = y + s->cfg->heightOffset - s->cfg->meshBaseY[mi] * sc;
    int i = *index;
    set->positions[i] = (Vector3){jx, y, jz};
    set->transforms[i] = MatrixMultiply(
        MatrixMultiply(MatrixScale(sc, sc, sc), MatrixRotateY(yaw)),
        MatrixTranslate(jx, baseY, jz));
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

  rlDisableBackfaceCulling(); // grass cards are two-sided
  for (int li = 0; li < f->layerCount; li++) {
    BrushFoliageLayer *L = &f->layers[li];
    float ds = f->distanceScale * f->qualityScale; // zoom * quality preset
    float draw = L->cfg.drawDistance * ds;
    float lod = L->cfg.lodDistance * ds;

    // Reset the per-model batch counts, then gather every chunk into them.
    for (int m = 0; m < L->meshCount; m++) { L->nearB.count[m] = 0; L->farB.count[m] = 0; }
    for (int ci = 0; ci < nv; ci++) {
      FoliageChunkHandle *H = (FoliageChunkHandle *)views[ci].handle;
      if (li >= H->setCount) continue;
      BrushFoliageCull(&H->sets[li], cam.position, cam.target, draw, lod,
                       &L->nearB, &L->farB);
    }
    int nc = 0, fc = 0;
    for (int m = 0; m < L->meshCount; m++) { nc += L->nearB.count[m]; fc += L->farB.count[m]; }
    L->lastNear = nc;
    L->lastFar = fc;
    if (nc == 0 && fc == 0) continue;

    float wind = f->windStrength * L->cfg.windStrength;
    SetShaderValue(f->shader, f->locTime, &f->time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(f->shader, f->locWindDir, &f->windDir, SHADER_UNIFORM_VEC2);
    SetShaderValue(f->shader, f->locWindStr, &wind, SHADER_UNIFORM_FLOAT);
    float fadeStart = draw * 0.72f;
    float fadeNearEnd = 0.0f, cutoff = 0.3f;
    SetShaderValue(f->shader, f->locFadeStart, &fadeStart, SHADER_UNIFORM_FLOAT);
    SetShaderValue(f->shader, f->locFadeEnd, &draw, SHADER_UNIFORM_FLOAT);
    SetShaderValue(f->shader, f->locFadeNearEnd, &fadeNearEnd, SHADER_UNIFORM_FLOAT);
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

    // One (near) + one (far) instanced draw PER model variant.
    for (int m = 0; m < L->meshCount; m++) {
      if (L->nearB.count[m] > 0)
        DrawMeshInstanced(L->nearMesh[m], L->material[m], L->nearB.buf[m], L->nearB.count[m]);
      if (L->farB.count[m] > 0)
        DrawMeshInstanced(L->hasFar[m] ? L->farMesh[m] : L->nearMesh[m],
                          L->material[m], L->farB.buf[m], L->farB.count[m]);
    }
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
