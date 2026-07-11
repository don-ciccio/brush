/*******************************************************************************************
 *   b_sky.c - Procedural sky dome
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_assets.h"
#include "b_sky.h"

#include <raymath.h>
#include <rlgl.h>

typedef struct BrushSkyState {
  Model dome;
  Shader shader;
  int locTime;
  int locSunDir;
  int locWindDir;
  int locExposure;
  int locTurbidity;
  int locMoonDir;
  Vector2 windDir;
  float turbidity;
} BrushSkyState;

static BrushSkyState g_sky = {0};

void BrushSkyInit(void) {
  g_sky.shader = LoadShader(BrushEnginePath("engine/shaders/sky.vs"), BrushEnginePath("engine/shaders/sky.fs"));
  g_sky.locTime = GetShaderLocation(g_sky.shader, "uTime");
  g_sky.locSunDir = GetShaderLocation(g_sky.shader, "uSunDir");
  g_sky.locWindDir = GetShaderLocation(g_sky.shader, "uWindDir");
  g_sky.locExposure = GetShaderLocation(g_sky.shader, "uExposure");
  g_sky.locTurbidity = GetShaderLocation(g_sky.shader, "uTurbidity");
  g_sky.locMoonDir = GetShaderLocation(g_sky.shader, "uMoonDir");

  Mesh sphere = GenMeshSphere(1.0f, 16, 24);
  g_sky.dome = LoadModelFromMesh(sphere);
  g_sky.dome.materials[0].shader = g_sky.shader;

  g_sky.windDir = (Vector2){0.7f, 0.7f};
  g_sky.turbidity = 2.5f;

  float exposure = 1.0f;
  SetShaderValue(g_sky.shader, g_sky.locExposure, &exposure,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_sky.shader, g_sky.locTurbidity, &g_sky.turbidity,
                 SHADER_UNIFORM_FLOAT);
}

void BrushSkyShutdown(void) {
  UnloadModel(g_sky.dome); // unloads the mesh; material shader freed below
  UnloadShader(g_sky.shader);
}

void BrushSkySetWind(Vector2 windDir) { g_sky.windDir = windDir; }

void BrushSkySetTurbidity(float turbidity) {
  g_sky.turbidity = turbidity;
  SetShaderValue(g_sky.shader, g_sky.locTurbidity, &g_sky.turbidity,
                 SHADER_UNIFORM_FLOAT);
}

void BrushSkyDraw(Camera3D camera, Vector3 sunDir, Vector3 moonDir) {
  float t = (float)GetTime();
  SetShaderValue(g_sky.shader, g_sky.locTime, &t, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_sky.shader, g_sky.locSunDir, &sunDir, SHADER_UNIFORM_VEC3);
  SetShaderValue(g_sky.shader, g_sky.locMoonDir, &moonDir,
                 SHADER_UNIFORM_VEC3);
  SetShaderValue(g_sky.shader, g_sky.locWindDir, &g_sky.windDir,
                 SHADER_UNIFORM_VEC2);

  // Camera sits inside the dome: draw back faces, keep depth writes off so
  // the far-plane trick (vs: gl_Position = clip.xyww) never occludes anything.
  rlDrawRenderBatchActive();
  rlDisableBackfaceCulling();
  rlDisableDepthMask();
  DrawModel(g_sky.dome, camera.position, 1.0f, WHITE);
  rlDrawRenderBatchActive();
  rlEnableBackfaceCulling();
  rlEnableDepthMask();
}
