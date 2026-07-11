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

typedef struct BrushShadow {
  RenderTexture2D map[BRUSH_SHADOW_CASCADES]; // depth-only render targets
  Camera3D lightCam;   // orthographic camera reused per cascade pass
  Matrix lightVP[BRUSH_SHADOW_CASCADES]; // captured during each depth pass
  float splitFar[BRUSH_SHADOW_CASCADES]; // cascade far distances from the
                                         // view camera (m); ascending
  int resolution;      // per-cascade shadow map size (square)
  int slot;            // first texture unit; cascade i binds slot + i
  float softness;      // PCSS light size (texels); higher = softer penumbra
  bool ready;
} BrushShadow;

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
