/*******************************************************************************************
 *   b_render.c - Layered render pipeline
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_render.h"
#include "b_post.h"
#include "b_sky.h"

#include <raymath.h>
#include <rlgl.h>
#include <stdlib.h>

#define BRUSH_MAX_DRAWS_PER_LAYER 1024

typedef struct BrushDrawCmd {
  Model *model;
  Matrix transform;
  Color tint;
  float sortKey; // camera distance, used for transparent back-to-front
} BrushDrawCmd;

typedef struct BrushRenderState {
  Shader lit;
  int locSunDir;
  int locSunColor;
  int locAmbient;
  int locLayerView;
  int locSpecStrength;
  int locLinearize;

  Vector3 sunDir;
  Vector3 sunColor;
  float ambient;
  bool skyEnabled;

  BrushLayerView layerView;

  BrushPost post;
  bool postEnabled;

  BrushDrawCmd cmds[BRUSH_LAYER_COUNT][BRUSH_MAX_DRAWS_PER_LAYER];
  int cmdCount[BRUSH_LAYER_COUNT];
} BrushRenderState;

static BrushRenderState g_r = {0};

void BrushRenderInit(int width, int height, float renderScale) {
  g_r.lit = LoadShader("engine/shaders/lit.vs", "engine/shaders/lit.fs");
  g_r.lit.locs[SHADER_LOC_VECTOR_VIEW] =
      GetShaderLocation(g_r.lit, "viewPos");
  g_r.locSunDir = GetShaderLocation(g_r.lit, "uSunDir");
  g_r.locSunColor = GetShaderLocation(g_r.lit, "uSunColor");
  g_r.locAmbient = GetShaderLocation(g_r.lit, "uAmbient");
  g_r.locLayerView = GetShaderLocation(g_r.lit, "uLayerView");
  g_r.locSpecStrength = GetShaderLocation(g_r.lit, "uSpecStrength");
  g_r.locLinearize = GetShaderLocation(g_r.lit, "uLinearize");

  // Pleasant default morning sun; games override via BrushSetSun.
  BrushSetSun((Vector3){0.45f, 0.55f, 0.35f},
              (Vector3){1.0f, 0.96f, 0.88f}, 0.38f);

  float spec = 0.35f;
  SetShaderValue(g_r.lit, g_r.locSpecStrength, &spec, SHADER_UNIFORM_FLOAT);

  g_r.skyEnabled = true;
  g_r.layerView = BRUSH_VIEW_FINAL;
  BrushSkyInit();

  BrushPostInit(&g_r.post, width, height, renderScale);
  g_r.postEnabled = (getenv("BRUSH_NO_POST") == NULL);

  TraceLog(LOG_INFO, "BRUSH: render pipeline ready (%d layers)",
           BRUSH_LAYER_COUNT);
}

void BrushRenderShutdown(void) {
  BrushPostUnload(&g_r.post);
  BrushSkyShutdown();
  UnloadShader(g_r.lit);
}

Shader BrushGetLitShader(void) { return g_r.lit; }

void BrushSetSun(Vector3 dir, Vector3 color, float ambient) {
  g_r.sunDir = Vector3Normalize(dir);
  g_r.sunColor = color;
  g_r.ambient = ambient;
  SetShaderValue(g_r.lit, g_r.locSunDir, &g_r.sunDir, SHADER_UNIFORM_VEC3);
  SetShaderValue(g_r.lit, g_r.locSunColor, &g_r.sunColor,
                 SHADER_UNIFORM_VEC3);
  SetShaderValue(g_r.lit, g_r.locAmbient, &g_r.ambient, SHADER_UNIFORM_FLOAT);
}

Vector3 BrushGetSunDir(void) { return g_r.sunDir; }

void BrushSetSkyEnabled(bool enabled) { g_r.skyEnabled = enabled; }

void BrushRenderSubmit(BrushLayer layer, Model *model, Matrix transform,
                       Color tint) {
  if (layer < 0 || layer >= BRUSH_LAYER_COUNT) return;
  int *count = &g_r.cmdCount[layer];
  if (*count >= BRUSH_MAX_DRAWS_PER_LAYER) return;
  g_r.cmds[layer][*count] =
      (BrushDrawCmd){.model = model, .transform = transform, .tint = tint};
  (*count)++;
}

// Draw a submitted command: the submitted matrix becomes the model transform
// for exactly this draw (the model's own transform is saved/restored).
static void DrawCmd(const BrushDrawCmd *cmd) {
  Matrix saved = cmd->model->transform;
  cmd->model->transform = cmd->transform;
  DrawModel(*cmd->model, (Vector3){0, 0, 0}, 1.0f, cmd->tint);
  cmd->model->transform = saved;
}

static int CompareFarToNear(const void *a, const void *b) {
  float da = ((const BrushDrawCmd *)a)->sortKey;
  float db = ((const BrushDrawCmd *)b)->sortKey;
  return (da < db) - (da > db); // descending: far first
}

void BrushRenderExecute(Camera3D camera) {
  // Per-frame lit-shader state.
  float camPos[3] = {camera.position.x, camera.position.y, camera.position.z};
  SetShaderValue(g_r.lit, g_r.lit.locs[SHADER_LOC_VECTOR_VIEW], camPos,
                 SHADER_UNIFORM_VEC3);
  int view = (int)g_r.layerView;
  SetShaderValue(g_r.lit, g_r.locLayerView, &view, SHADER_UNIFORM_INT);

  // SHADOW: reserved. Submissions are accepted so games can already tag
  // casters; the sun depth pass executes here once shadow mapping is ported.

  // With post on, the layer stack renders into the linear HDR target and
  // b_post composites to the backbuffer. Debug layer views bypass post so the
  // isolated terms stay readable. With post off (F3), surfaces draw straight
  // to the LDR backbuffer — a diagnostic fallback: linear output looks flat.
  bool usePost = g_r.postEnabled && g_r.post.ready &&
                 g_r.layerView == BRUSH_VIEW_FINAL;
  float linearize = usePost ? 1.0f : 0.0f;
  SetShaderValue(g_r.lit, g_r.locLinearize, &linearize, SHADER_UNIFORM_FLOAT);
  if (usePost) BrushPostBeginScene(&g_r.post);

  BeginMode3D(camera);

  // OPAQUE — the color pass (albedo * (ambient + diffuse) + specular).
  for (int i = 0; i < g_r.cmdCount[BRUSH_LAYER_OPAQUE]; i++)
    DrawCmd(&g_r.cmds[BRUSH_LAYER_OPAQUE][i]);

  // SKY — after opaque so the far-plane dome is early-Z rejected everywhere
  // geometry already wrote depth (only visible sky pixels pay for clouds).
  if (g_r.skyEnabled && g_r.layerView == BRUSH_VIEW_FINAL)
    BrushSkyDraw(camera, g_r.sunDir);

  // TRANSPARENT — back-to-front, depth-test on, depth-write off.
  int tCount = g_r.cmdCount[BRUSH_LAYER_TRANSPARENT];
  if (tCount > 0) {
    BrushDrawCmd *cmds = g_r.cmds[BRUSH_LAYER_TRANSPARENT];
    for (int i = 0; i < tCount; i++) {
      Vector3 p = {cmds[i].transform.m12, cmds[i].transform.m13,
                   cmds[i].transform.m14};
      cmds[i].sortKey = Vector3DistanceSqr(p, camera.position);
    }
    qsort(cmds, (size_t)tCount, sizeof(BrushDrawCmd), CompareFarToNear);
    rlDrawRenderBatchActive();
    rlDisableDepthMask();
    for (int i = 0; i < tCount; i++) DrawCmd(&cmds[i]);
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
  }

  // VOLUME: reserved for volumetric fog / god rays.

  EndMode3D();

  if (usePost) {
    BrushPostEndScene(&g_r.post);
    BrushPostRun(&g_r.post, (float)GetTime());
  }

  for (int i = 0; i < BRUSH_LAYER_COUNT; i++) g_r.cmdCount[i] = 0;
}

void BrushRenderTogglePost(void) {
  g_r.postEnabled = !g_r.postEnabled;
  TraceLog(LOG_INFO, "BRUSH: post pipeline %s",
           g_r.postEnabled ? "ON" : "OFF");
}

bool BrushRenderIsPostEnabled(void) { return g_r.postEnabled && g_r.post.ready; }

struct BrushPost *BrushRenderGetPost(void) {
  return g_r.post.ready ? &g_r.post : NULL;
}

void BrushRenderCycleLayerView(void) {
  g_r.layerView = (BrushLayerView)((g_r.layerView + 1) % BRUSH_VIEW_COUNT);
  TraceLog(LOG_INFO, "BRUSH: layer view -> %s", BrushRenderLayerViewName());
}

BrushLayerView BrushRenderGetLayerView(void) { return g_r.layerView; }

const char *BrushRenderLayerViewName(void) {
  switch (g_r.layerView) {
  case BRUSH_VIEW_FINAL: return "final";
  case BRUSH_VIEW_ALBEDO: return "albedo";
  case BRUSH_VIEW_DIFFUSE: return "diffuse light";
  case BRUSH_VIEW_SPECULAR: return "specular";
  case BRUSH_VIEW_NORMALS: return "normals";
  default: return "?";
  }
}
