/*******************************************************************************************
 *   b_meshlod.c - Runtime mesh decimation for instanced foliage LODs
 *
 *   Area-based triangle importance scoring + rebuild. For foliage meshes
 *   (grass tufts, clumps) where distant triangles add little on screen but
 *   still cost GPU fill:
 *
 *     1. Score each triangle by area x silhouette contribution x tip bias.
 *     2. Sort triangles by score (most important first).
 *     3. Keep the top `targetRatio` fraction.
 *     4. Rebuild the mesh over the vertices those triangles reference.
 *
 *   UVs, normals, and vertex colors are preserved exactly (compacted, never
 *   interpolated). Ported from the donor game's mesh_decimate; see
 *   docs/foliage-plan.md, roadmap v1 #7 (Phase 1).
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_meshlod.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// One triangle's visual-importance score, tagged with its original index so
// the sort can reorder without moving geometry.
typedef struct {
  int triIndex;
  float score;
} TriScore;

// qsort comparator — descending (most important first).
static int CompareTriScore(const void *a, const void *b) {
  float sa = ((const TriScore *)a)->score;
  float sb = ((const TriScore *)b)->score;
  if (sb > sa) return 1;
  if (sb < sa) return -1;
  return 0;
}

static void Cross(const float *a, const float *b, float *out) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

int BrushMeshDecimate(Mesh *mesh, float targetRatio) {
  if (!mesh || mesh->vertexCount == 0 || mesh->triangleCount == 0)
    return mesh ? mesh->vertexCount : 0;

  if (targetRatio >= 1.0f) return mesh->vertexCount; // no reduction needed
  if (targetRatio < 0.02f) targetRatio = 0.02f;      // safety floor (~2%)

  int triCount = mesh->triangleCount;
  int keepCount = (int)(triCount * targetRatio);
  if (keepCount < 10) keepCount = 10;
  if (keepCount >= triCount) return mesh->vertexCount;

  // Mesh Y bounds for the tip bias (grass tips define the silhouette).
  float minY = 1e9f, maxY = -1e9f;
  for (int i = 0; i < mesh->vertexCount; i++) {
    float y = mesh->vertices[i * 3 + 1];
    if (y < minY) minY = y;
    if (y > maxY) maxY = y;
  }
  float heightRange = (maxY > minY) ? (maxY - minY) : 1.0f;

  TriScore *scores = (TriScore *)MemAlloc(sizeof(TriScore) * triCount);

  for (int t = 0; t < triCount; t++) {
    scores[t].triIndex = t;

    int i0, i1, i2;
    if (mesh->indices) {
      i0 = mesh->indices[t * 3 + 0];
      i1 = mesh->indices[t * 3 + 1];
      i2 = mesh->indices[t * 3 + 2];
    } else {
      i0 = t * 3 + 0;
      i1 = t * 3 + 1;
      i2 = t * 3 + 2;
    }

    float *v0 = &mesh->vertices[i0 * 3];
    float *v1 = &mesh->vertices[i1 * 3];
    float *v2 = &mesh->vertices[i2 * 3];

    float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
    float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};

    float cross[3];
    Cross(e1, e2, cross);
    float normalLen = sqrtf(cross[0] * cross[0] + cross[1] * cross[1] +
                            cross[2] * cross[2]);
    float area = 0.5f * normalLen;

    // Tip triangles score higher (0 = base, 1 = tip).
    float avgY = (v0[1] + v1[1] + v2[1]) / 3.0f;
    float heightFactor = (avgY - minY) / heightRange;

    // Silhouette contribution: normals facing sideways (XZ) define the outline.
    float silhouette = 1.0f;
    if (normalLen > 0.001f) {
      float horizComponent =
          sqrtf(cross[0] * cross[0] + cross[2] * cross[2]) / normalLen;
      silhouette = 0.5f + horizComponent * 0.5f; // 0.5 .. 1.0
    }

    scores[t].score = area * silhouette * (0.6f + heightFactor * 0.4f);
  }

  // Most important triangles first.
  qsort(scores, triCount, sizeof(TriScore), CompareTriScore);

  // Map each referenced old vertex to a compact new index.
  int *vertexUsed = (int *)MemAlloc(sizeof(int) * mesh->vertexCount);
  memset(vertexUsed, -1, sizeof(int) * mesh->vertexCount);

  int newVertCount = 0;
  for (int k = 0; k < keepCount; k++) {
    int t = scores[k].triIndex;
    int i0, i1, i2;
    if (mesh->indices) {
      i0 = mesh->indices[t * 3 + 0];
      i1 = mesh->indices[t * 3 + 1];
      i2 = mesh->indices[t * 3 + 2];
    } else {
      i0 = t * 3 + 0;
      i1 = t * 3 + 1;
      i2 = t * 3 + 2;
    }
    if (vertexUsed[i0] == -1) vertexUsed[i0] = newVertCount++;
    if (vertexUsed[i1] == -1) vertexUsed[i1] = newVertCount++;
    if (vertexUsed[i2] == -1) vertexUsed[i2] = newVertCount++;
  }

  float *newVerts = (float *)MemAlloc(sizeof(float) * newVertCount * 3);
  float *newTexcoords =
      mesh->texcoords ? (float *)MemAlloc(sizeof(float) * newVertCount * 2)
                      : NULL;
  float *newNormals =
      mesh->normals ? (float *)MemAlloc(sizeof(float) * newVertCount * 3)
                    : NULL;
  unsigned char *newColors =
      mesh->colors
          ? (unsigned char *)MemAlloc(sizeof(unsigned char) * newVertCount * 4)
          : NULL;
  unsigned short *newIndices =
      (unsigned short *)MemAlloc(sizeof(unsigned short) * keepCount * 3);

  // Compact vertex data into the new layout.
  for (int i = 0; i < mesh->vertexCount; i++) {
    int ni = vertexUsed[i];
    if (ni < 0) continue;

    newVerts[ni * 3 + 0] = mesh->vertices[i * 3 + 0];
    newVerts[ni * 3 + 1] = mesh->vertices[i * 3 + 1];
    newVerts[ni * 3 + 2] = mesh->vertices[i * 3 + 2];

    if (newTexcoords) {
      newTexcoords[ni * 2 + 0] = mesh->texcoords[i * 2 + 0];
      newTexcoords[ni * 2 + 1] = mesh->texcoords[i * 2 + 1];
    }
    if (newNormals) {
      newNormals[ni * 3 + 0] = mesh->normals[i * 3 + 0];
      newNormals[ni * 3 + 1] = mesh->normals[i * 3 + 1];
      newNormals[ni * 3 + 2] = mesh->normals[i * 3 + 2];
    }
    if (newColors) {
      newColors[ni * 4 + 0] = mesh->colors[i * 4 + 0];
      newColors[ni * 4 + 1] = mesh->colors[i * 4 + 1];
      newColors[ni * 4 + 2] = mesh->colors[i * 4 + 2];
      newColors[ni * 4 + 3] = mesh->colors[i * 4 + 3];
    }
  }

  // Remap the kept triangles onto the compact vertices.
  for (int k = 0; k < keepCount; k++) {
    int t = scores[k].triIndex;
    int i0, i1, i2;
    if (mesh->indices) {
      i0 = mesh->indices[t * 3 + 0];
      i1 = mesh->indices[t * 3 + 1];
      i2 = mesh->indices[t * 3 + 2];
    } else {
      i0 = t * 3 + 0;
      i1 = t * 3 + 1;
      i2 = t * 3 + 2;
    }
    newIndices[k * 3 + 0] = (unsigned short)vertexUsed[i0];
    newIndices[k * 3 + 1] = (unsigned short)vertexUsed[i1];
    newIndices[k * 3 + 2] = (unsigned short)vertexUsed[i2];
  }

  int origVerts = mesh->vertexCount;
  int origTris = mesh->triangleCount;

  MemFree(mesh->vertices);
  mesh->vertices = newVerts;
  if (mesh->texcoords) { MemFree(mesh->texcoords); mesh->texcoords = newTexcoords; }
  if (mesh->normals)   { MemFree(mesh->normals);   mesh->normals = newNormals; }
  if (mesh->colors)    { MemFree(mesh->colors);    mesh->colors = newColors; }
  if (mesh->indices)   { MemFree(mesh->indices); }
  mesh->indices = newIndices;

  // We don't carry these through the LOD; drop them so the mesh stays consistent.
  if (mesh->texcoords2) { MemFree(mesh->texcoords2); mesh->texcoords2 = NULL; }
  if (mesh->tangents)   { MemFree(mesh->tangents);   mesh->tangents = NULL; }

  mesh->vertexCount = newVertCount;
  mesh->triangleCount = keepCount;

  MemFree(scores);
  MemFree(vertexUsed);

  TraceLog(LOG_INFO,
           "BrushMeshDecimate: %d verts, %d tris -> %d verts, %d tris (%.0f%% kept)",
           origVerts, origTris, newVertCount, keepCount, targetRatio * 100.0f);

  return newVertCount;
}
