/*******************************************************************************************
 *   b_meshlod.h - Runtime mesh decimation for instanced foliage LODs
 *
 *   A lightweight edge-collapse-free decimator: score each triangle by visual
 *   importance (area x silhouette contribution x tip bias), sort, and rebuild
 *   the mesh keeping the top N%. UVs, normals, and vertex colors are preserved
 *   exactly (no interpolation) — the vertex set is simply compacted to the
 *   triangles that survive.
 *
 *   Full QEM (Garland-Heckbert, "Surface Simplification Using Quadric Error
 *   Metrics", 1997) is overkill for the small, build-once foliage meshes this
 *   feeds; naive stride-skip loses silhouette unpredictably. This sits in
 *   between and is cheap enough to run at layer-create.
 *
 *   Pure CPU — no GL — so it is safe to call off the main thread. The caller
 *   uploads the result (UploadMesh) afterward. See docs/foliage-plan.md.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_MESHLOD_H
#define B_MESHLOD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <raylib.h>

// Decimate a raylib Mesh in-place, keeping `targetRatio` of its triangles
// (0.5 = keep 50%, 0.3 = 30%, ...). Clamped to [0.02, 1.0]; >=1 is a no-op.
// A floor of 10 triangles is enforced so tiny meshes stay valid. Preserves
// vertex positions, UVs, normals, and colors exactly; drops texcoords2 and
// tangents (not needed by the foliage path). The mesh's CPU arrays are freed
// and replaced — do NOT call on a mesh whose GPU buffers are already uploaded
// unless you re-upload. Returns the new vertex count.
int BrushMeshDecimate(Mesh *mesh, float targetRatio);

#ifdef __cplusplus
}
#endif

#endif // B_MESHLOD_H
