/*******************************************************************************************
 *   b_shadow.c - Cascaded directional sun shadow mapping (see b_shadow.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_shadow.h"

#include <math.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdio.h>
#include <stdlib.h>

// Depth-only framebuffer (no color attachment) for a shadow map.
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
  sh->softness = 4.0f;
  const char *softEnv = getenv("BRUSH_SHADOW_SOFT");
  if (softEnv != NULL) sh->softness = (float)atof(softEnv);

  // Cascade far distances from the view camera. The near cascade covers the
  // character/gameplay bubble at high texel density; the far one covers the
  // whole mid-distance so shadows never just stop. BRUSH_CSM_SPLITS="a,b,c"
  // overrides for tuning.
  sh->splitFar[0] = 24.0f;
  sh->splitFar[1] = 72.0f;
  sh->splitFar[2] = 220.0f;
  const char *splitEnv = getenv("BRUSH_CSM_SPLITS");
  if (splitEnv != NULL) {
    float a, b, c;
    if (sscanf(splitEnv, "%f,%f,%f", &a, &b, &c) == 3) {
      sh->splitFar[0] = a;
      sh->splitFar[1] = b;
      sh->splitFar[2] = c;
    }
  }

  sh->ready = true;
  for (int i = 0; i < BRUSH_SHADOW_CASCADES; i++) {
    sh->map[i] = LoadShadowmapRenderTexture(resolution, resolution);
    sh->lightVP[i] = MatrixIdentity();
    if (sh->map[i].id == 0 || sh->map[i].depth.id == 0) sh->ready = false;
  }

  sh->lightCam.up = (Vector3){0.0f, 1.0f, 0.0f};
  sh->lightCam.projection = CAMERA_ORTHOGRAPHIC;
}

void BrushShadowUnload(BrushShadow *sh) {
  for (int i = 0; i < BRUSH_SHADOW_CASCADES; i++) {
    if (sh->map[i].id > 0) {
      rlUnloadFramebuffer(sh->map[i].id);
      if (sh->map[i].depth.id > 0) rlUnloadTexture(sh->map[i].depth.id);
    }
  }
  *sh = (BrushShadow){0};
}

void BrushShadowBeginCascade(BrushShadow *sh, int i, Vector3 sunDir,
                             Camera3D viewCam) {
  Vector3 dir = Vector3Normalize(sunDir);

  // --- Frustum slice [near, far] of the VIEW camera, in world space -------
  float near = (i == 0) ? 0.05f : sh->splitFar[i - 1];
  float far = sh->splitFar[i];

  Vector3 fwd = Vector3Normalize(Vector3Subtract(viewCam.target,
                                                 viewCam.position));
  Vector3 upRefCam = (fabsf(fwd.y) > 0.99f) ? (Vector3){0, 0, 1}
                                            : (Vector3){0, 1, 0};
  Vector3 camRight = Vector3Normalize(Vector3CrossProduct(fwd, upRefCam));
  Vector3 camUp = Vector3CrossProduct(camRight, fwd);

  float aspect = (float)GetScreenWidth() / (float)GetScreenHeight();
  float tanHalf = tanf(viewCam.fovy * 0.5f * DEG2RAD);

  // Bounding SPHERE of the slice's 8 corners: rotation-invariant, so the
  // ortho box size stays constant while the camera looks around — a
  // prerequisite for texel snapping to eliminate edge crawl.
  Vector3 corners[8];
  int n = 0;
  for (int p = 0; p < 2; p++) {
    float d = (p == 0) ? near : far;
    float hh = tanHalf * d, hw = hh * aspect;
    Vector3 c = Vector3Add(viewCam.position, Vector3Scale(fwd, d));
    for (int sy = -1; sy <= 1; sy += 2)
      for (int sx = -1; sx <= 1; sx += 2)
        corners[n++] = Vector3Add(
            c, Vector3Add(Vector3Scale(camRight, hw * (float)sx),
                          Vector3Scale(camUp, hh * (float)sy)));
  }
  Vector3 center = {0};
  for (int k = 0; k < 8; k++) center = Vector3Add(center, corners[k]);
  center = Vector3Scale(center, 1.0f / 8.0f);
  float radius = 0.0f;
  for (int k = 0; k < 8; k++) {
    float d = Vector3Distance(center, corners[k]);
    if (d > radius) radius = d;
  }
  radius = ceilf(radius); // whole metres: box size never jitters

  // --- Light-space texel snap of the sphere centre -------------------------
  Vector3 upRef = (fabsf(dir.y) > 0.99f) ? (Vector3){0, 0, 1}
                                         : (Vector3){0, 1, 0};
  Vector3 right = Vector3Normalize(Vector3CrossProduct(upRef, dir));
  Vector3 up = Vector3CrossProduct(dir, right);
  float orthoSize = radius * 2.0f;
  float texelSize = orthoSize / (float)sh->resolution;
  float rx = floorf(Vector3DotProduct(center, right) / texelSize) * texelSize;
  float uy = floorf(Vector3DotProduct(center, up) / texelSize) * texelSize;
  float dz = Vector3DotProduct(center, dir);
  Vector3 snapped =
      Vector3Add(Vector3Add(Vector3Scale(right, rx), Vector3Scale(up, uy)),
                 Vector3Scale(dir, dz));

  sh->lightCam.target = snapped;
  // Back off past the slice radius so casters between the sun and the slice
  // (tall terrain, buildings behind the camera) still land in the map.
  sh->lightCam.position =
      Vector3Add(snapped, Vector3Scale(dir, radius + 120.0f));
  sh->lightCam.up = up;
  sh->lightCam.fovy = orthoSize; // ortho: fovy is the world-space box size

  BeginTextureMode(sh->map[i]);
  ClearBackground(WHITE); // clears depth to far
  // The map must not be bound as a sampler while we render into it.
  rlActiveTextureSlot(sh->slot + i);
  rlDisableTexture();
  BeginMode3D(sh->lightCam);
  // Capture the exact matrices raylib uses so the shader test matches.
  Matrix view = rlGetMatrixModelview();
  Matrix proj = rlGetMatrixProjection();
  sh->lightVP[i] = MatrixMultiply(view, proj);
}

void BrushShadowEnd(BrushShadow *sh) {
  (void)sh;
  EndMode3D();
  EndTextureMode();
}

void BrushShadowBindMaps(BrushShadow *sh) {
  for (int i = 0; i < BRUSH_SHADOW_CASCADES; i++) {
    rlActiveTextureSlot(sh->slot + i);
    rlEnableTexture(sh->map[i].depth.id);
  }
  rlActiveTextureSlot(0);
}
