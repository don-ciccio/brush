/*******************************************************************************************
 *   b_sky.h - Procedural sky dome (atmospheric scattering + volumetric clouds)
 *
 *   Fully procedural (no textures): Rayleigh/Mie scattering, FBM cumulus
 *   clouds with Beer-Lambert self-shadowing and silver lining, a stable
 *   twinkling star field for night. The dome is a camera-centered sphere
 *   forced to the far plane in the vertex shader, drawn after opaque geometry
 *   so early-Z culls covered pixels.
 *
 *   Owned by the render pipeline (BRUSH_LAYER_SKY); games normally only
 *   toggle it via BrushSetSkyEnabled.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_SKY_H
#define B_SKY_H

#include <raylib.h>

void BrushSkyInit(void);
void BrushSkyShutdown(void);

// Draw the dome centered on the camera. `sunDir` points toward the sun.
void BrushSkyDraw(Camera3D camera, Vector3 sunDir);

// Cloud drift (world XZ direction * speed) and haze factor.
void BrushSkySetWind(Vector2 windDir);
void BrushSkySetTurbidity(float turbidity);

#endif // B_SKY_H
