/*******************************************************************************************
 *   b_shadow.h - Cascaded directional sun shadow mapping (CSM)
 *
 *   Three depth-only passes from an orthographic camera up the sun
 *   direction, each fitted around a slice of the VIEW frustum: a tight box
 *   near the camera (crisp contact shadows), wider boxes further out (the
 *   whole visible range casts). The lit shader picks the cascade by the
 *   fragment's distance from the camera and runs the same PCSS test
 *   (blocker search -> penumbra-widening PCF) against that cascade's map.
 *
 *   Stability: each cascade fits the BOUNDING SPHERE of its frustum slice
 *   (so the box size doesn't change as the camera rotates) and the box
 *   centre is snapped to shadow-map texels in light space (so a moving
 *   camera re-rasterizes static edges into the same texels — no crawl).
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_SHADOW_H
#define B_SHADOW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <raylib.h>
#include <stdbool.h>

#define BRUSH_SHADOW_CASCADES 3

// The three cascades share ONE depth texture — a 2x2 square atlas (the 4th tile
// is unused) — so the lit/foliage shaders spend a SINGLE texture unit on shadows
// instead of three (freeing units under the hard 16-unit fragment limit). Each
// cascade renders into its tile via a viewport; the shader remaps [0,1] cascade
// UV into the tile (see BrushShadowCascadeTile / the shaders). A square atlas
// keeps BeginMode3D's aspect at 1.0, so the ortho boxes stay square with no
// manual projection maths.
typedef struct BrushShadow {
  RenderTexture2D atlas; // one depth-only target, 2*resolution square
  Camera3D lightCam;   // orthographic camera reused per cascade pass
  Matrix lightVP[BRUSH_SHADOW_CASCADES]; // captured during each depth pass
  float splitFar[BRUSH_SHADOW_CASCADES]; // cascade far distances from the
                                         // view camera (m); ascending
  int resolution;      // per-cascade tile size (square); atlas is 2x this
  int slot;            // the single texture unit the atlas binds to
  float softness;      // PCSS light size (texels); higher = softer penumbra
  bool ready;
} BrushShadow;

// Atlas tile origin (bottom-left, in [0,1] atlas UV) for cascade i, 2x2 layout.
// Shared by the C bind path and mirrored in the shaders.
static inline Vector2 BrushShadowCascadeTile(int i) {
  return (Vector2){(float)(i & 1) * 0.5f, (float)(i >> 1) * 0.5f};
}

void BrushShadowInit(BrushShadow *sh, int resolution);
void BrushShadowUnload(BrushShadow *sh);

// Begin cascade `i`'s depth pass: fit the ortho box around the view camera's
// [splitFar[i-1], splitFar[i]] frustum slice (sphere fit, texel-snapped),
// open the depth target and capture lightVP[i]. Draw the casters between
// Begin and End with their normal draw calls — only depth is kept.
void BrushShadowBeginCascade(BrushShadow *sh, int i, Vector3 sunDir,
                             Camera3D viewCam);
void BrushShadowEnd(BrushShadow *sh);

// Bind every cascade's depth texture on its reserved slot (call before
// drawing receivers).
void BrushShadowBindMaps(BrushShadow *sh);

#ifdef __cplusplus
}
#endif

#endif // B_SHADOW_H
