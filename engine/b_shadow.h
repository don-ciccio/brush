/*******************************************************************************************
 *   b_shadow.h - Directional sun shadow mapping
 *
 *   A single depth-only pass from an orthographic camera up the sun
 *   direction. The renderer draws every BRUSH_LAYER_SHADOW submission into
 *   the map, then the lit shader tests fragments against it with PCSS
 *   (blocker search -> penumbra-widening PCF), so shadows are sharp at
 *   contact and soften with distance from the caster.
 *
 *   The ortho box FOLLOWS the view target and is snapped to shadow-map
 *   texels in light space: without snapping, a moving camera re-rasterizes
 *   the same edges into different texels every frame and shadow edges crawl.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_SHADOW_H
#define B_SHADOW_H

#include <raylib.h>
#include <stdbool.h>

typedef struct BrushShadow {
  RenderTexture2D map; // depth-only render target
  Camera3D lightCam;   // orthographic camera from the sun direction
  Matrix lightVP;      // light view-projection captured during the depth pass
  int resolution;      // shadow map size (square)
  int slot;            // texture unit the map binds to (above material maps)
  float orthoSize;     // world-space coverage of the box (metres, square)
  float softness;      // PCSS light size (texels); higher = softer penumbra
  bool ready;
} BrushShadow;

void BrushShadowInit(BrushShadow *sh, int resolution);
void BrushShadowUnload(BrushShadow *sh);

// Begin the depth pass: aim the ortho camera down `sunDir` (points toward the
// sun) at `focus` (texel-snapped), open the depth target and capture lightVP.
// Draw the casters between Begin and End with their normal draw calls — only
// depth is kept.
void BrushShadowBegin(BrushShadow *sh, Vector3 sunDir, Vector3 focus);
void BrushShadowEnd(BrushShadow *sh);

// Bind the depth texture on its reserved slot (call before drawing receivers).
void BrushShadowBindMap(BrushShadow *sh);

#endif // B_SHADOW_H
