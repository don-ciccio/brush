/*******************************************************************************************
 *   b_shadow.c - Directional sun shadow mapping (see b_shadow.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_shadow.h"

#include <math.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdlib.h>

// Depth-only framebuffer (no color attachment) for the shadow map.
static RenderTexture2D LoadShadowmapRenderTexture(int width, int height) {
  RenderTexture2D target = {0};
  target.id = rlLoadFramebuffer();
  if (target.id > 0) {
    rlEnableFramebuffer(target.id);
    target.depth.id = rlLoadTextureDepth(width, height, false);
    target.depth.width = width;
    target.depth.height = height;
    target.depth.format = 19; // DEPTH_COMPONENT_24BIT
    target.depth.mipmaps = 1;
    rlFramebufferAttach(target.id, target.depth.id, RL_ATTACHMENT_DEPTH,
                        RL_ATTACHMENT_TEXTURE2D, 0);
    if (rlFramebufferComplete(target.id))
      TraceLog(LOG_INFO, "BRUSH shadow: depth FBO [ID %i] created (%dx%d)",
               target.id, width, height);
    rlDisableFramebuffer();
  } else {
    TraceLog(LOG_WARNING, "BRUSH shadow: failed to create depth FBO");
  }
  target.texture.width = width;
  target.texture.height = height;
  return target;
}

void BrushShadowInit(BrushShadow *sh, int resolution) {
  *sh = (BrushShadow){0};
  const char *resEnv = getenv("BRUSH_SHADOW_RES");
  if (resEnv != NULL) resolution = atoi(resEnv);
  if (resolution <= 0) resolution = 2048;
  sh->resolution = resolution;
  sh->slot = 10; // above the material map slots (diffuse/normal/etc.)
  sh->orthoSize = 60.0f;
  sh->softness = 4.0f;
  const char *softEnv = getenv("BRUSH_SHADOW_SOFT");
  if (softEnv != NULL) sh->softness = (float)atof(softEnv);

  sh->map = LoadShadowmapRenderTexture(resolution, resolution);
  sh->lightCam.up = (Vector3){0.0f, 1.0f, 0.0f};
  sh->lightCam.projection = CAMERA_ORTHOGRAPHIC;
  sh->lightCam.fovy = sh->orthoSize; // ortho: fovy is the world-space size
  sh->lightVP = MatrixIdentity();
  sh->ready = (sh->map.id > 0 && sh->map.depth.id > 0);
}

void BrushShadowUnload(BrushShadow *sh) {
  if (sh->map.id > 0) {
    rlUnloadFramebuffer(sh->map.id); // depth texture goes with the FBO
  }
  *sh = (BrushShadow){0};
}

void BrushShadowBegin(BrushShadow *sh, Vector3 sunDir, Vector3 focus) {
  Vector3 dir = Vector3Normalize(sunDir);

  // Texel snapping: express the focus in the light's right/up basis, round to
  // whole shadow-map texels, and rebuild. The ortho box then slides in texel
  // steps, so a static edge always rasterizes into the same texels and shadow
  // edges don't crawl as the camera moves.
  Vector3 upRef = (fabsf(dir.y) > 0.99f) ? (Vector3){0, 0, 1}
                                         : (Vector3){0, 1, 0};
  Vector3 right = Vector3Normalize(Vector3CrossProduct(upRef, dir));
  Vector3 up = Vector3CrossProduct(dir, right);
  float texelSize = sh->orthoSize / (float)sh->resolution;
  float rx = floorf(Vector3DotProduct(focus, right) / texelSize) * texelSize;
  float uy = floorf(Vector3DotProduct(focus, up) / texelSize) * texelSize;
  float dz = Vector3DotProduct(focus, dir);
  Vector3 snapped =
      Vector3Add(Vector3Add(Vector3Scale(right, rx), Vector3Scale(up, uy)),
                 Vector3Scale(dir, dz));

  sh->lightCam.target = snapped;
  sh->lightCam.position = Vector3Add(snapped, Vector3Scale(dir, 100.0f));
  sh->lightCam.up = up;
  sh->lightCam.fovy = sh->orthoSize;

  BeginTextureMode(sh->map);
  ClearBackground(WHITE); // clears depth to far
  // The map must not be bound as a sampler while we render into it.
  rlActiveTextureSlot(sh->slot);
  rlDisableTexture();
  BeginMode3D(sh->lightCam);
  // Capture the exact matrices raylib uses so the shader test matches.
  Matrix view = rlGetMatrixModelview();
  Matrix proj = rlGetMatrixProjection();
  sh->lightVP = MatrixMultiply(view, proj);
}

void BrushShadowEnd(BrushShadow *sh) {
  (void)sh;
  EndMode3D();
  EndTextureMode();
}

void BrushShadowBindMap(BrushShadow *sh) {
  rlActiveTextureSlot(sh->slot);
  rlEnableTexture(sh->map.depth.id);
  rlActiveTextureSlot(0);
}
