/*******************************************************************************************
 *   b_render.c - Layered render pipeline
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_render.h"
#include "b_post.h"
#include "b_shadow.h"
#include "b_sky.h"
#include "b_tod.h"

#include <raymath.h>
#include <rlgl.h>
#include <stdlib.h>

#define BRUSH_MAX_DRAWS_PER_LAYER 1024

typedef struct BrushDrawCmd {
  Model *model;        // NULL for a raw mesh command
  Mesh mesh;           // used when model == NULL (streamed terrain, etc.)
  Material *material;   // used when model == NULL
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
  int locLightVP, locShadowMap, locShadowEnabled;
  int locShadowSoftness, locShadowTexel, locShadowStrength;

  Vector3 sunDir;   // the directional LIGHT (sun by day, moon at night)
  Vector3 sunColor;
  Vector3 ambient;  // ambient fill color (linear)
  Vector3 skySunDir; // the actual sun, even below the horizon (sky shading)
  Vector3 moonDir;
  bool skyEnabled;

  BrushLayerView layerView;

  BrushPost post;
  bool postEnabled;

  BrushShadow shadow;
  bool shadowsEnabled;

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
  g_r.locLightVP = GetShaderLocation(g_r.lit, "lightVP");
  g_r.locShadowMap = GetShaderLocation(g_r.lit, "shadowMap");
  g_r.locShadowEnabled = GetShaderLocation(g_r.lit, "uShadowEnabled");
  g_r.locShadowSoftness = GetShaderLocation(g_r.lit, "uShadowSoftness");
  g_r.locShadowTexel = GetShaderLocation(g_r.lit, "uShadowTexel");
  g_r.locShadowStrength = GetShaderLocation(g_r.lit, "uShadowStrength");

  // Pleasant default morning sun; games override via BrushSetSun or drive it
  // from a clock with BrushRenderApplyTimeOfDay.
  BrushSetSun((Vector3){0.45f, 0.55f, 0.35f}, (Vector3){1.0f, 0.96f, 0.88f},
              (Vector3){0.36f, 0.38f, 0.42f});
  g_r.moonDir = (Vector3){0.0f, -1.0f, 0.0f};

  float spec = 0.35f;
  SetShaderValue(g_r.lit, g_r.locSpecStrength, &spec, SHADER_UNIFORM_FLOAT);

  g_r.skyEnabled = true;
  g_r.layerView = BRUSH_VIEW_FINAL;
  BrushSkyInit();

  BrushPostInit(&g_r.post, width, height, renderScale);
  g_r.postEnabled = (getenv("BRUSH_NO_POST") == NULL);

  BrushShadowInit(&g_r.shadow, 2048);
  g_r.shadowsEnabled = (getenv("BRUSH_NO_SHADOW") == NULL);

  TraceLog(LOG_INFO, "BRUSH: render pipeline ready (%d layers)",
           BRUSH_LAYER_COUNT);
}

void BrushRenderShutdown(void) {
  BrushShadowUnload(&g_r.shadow);
  BrushPostUnload(&g_r.post);
  BrushSkyShutdown();
  UnloadShader(g_r.lit);
}

Shader BrushGetLitShader(void) { return g_r.lit; }

void BrushSetSun(Vector3 dir, Vector3 color, Vector3 ambient) {
  g_r.sunDir = Vector3Normalize(dir);
  g_r.sunColor = color;
  g_r.ambient = ambient;
  g_r.skySunDir = g_r.sunDir; // static-sun games: sky follows the light
  SetShaderValue(g_r.lit, g_r.locSunDir, &g_r.sunDir, SHADER_UNIFORM_VEC3);
  SetShaderValue(g_r.lit, g_r.locSunColor, &g_r.sunColor,
                 SHADER_UNIFORM_VEC3);
  SetShaderValue(g_r.lit, g_r.locAmbient, &g_r.ambient, SHADER_UNIFORM_VEC3);
}

void BrushRenderApplyTimeOfDay(const struct BrushTimeOfDay *tod) {
  Vector3 sunDir = BrushTodSunDir(tod);
  Vector3 moonDir = BrushTodMoonDir(tod);
  Vector3 sunColor = BrushTodSunColor(tod);
  Vector3 moonColor = BrushTodMoonColor(tod);

  // Single-light handover: the sun is the directional light while up; once it
  // sets, the moon (if risen) takes the same slot. The LUTs ramp both colors
  // through black at the horizon, so the swap never pops.
  bool moonlight = (sunDir.y <= 0.0f && moonDir.y > 0.0f);
  BrushSetSun(moonlight ? moonDir : sunDir,
              moonlight ? moonColor : sunColor, BrushTodAmbientColor(tod));

  // The sky always shades from the TRUE sun (scattering needs it below the
  // horizon too) plus the moon for night clouds/stars.
  g_r.skySunDir = sunDir;
  g_r.moonDir = moonDir;

  if (g_r.post.ready) g_r.post.exposure = BrushTodExposure(tod);
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

void BrushRenderSubmitMesh(BrushLayer layer, Mesh mesh, Material *material,
                           Matrix transform) {
  if (layer < 0 || layer >= BRUSH_LAYER_COUNT) return;
  int *count = &g_r.cmdCount[layer];
  if (*count >= BRUSH_MAX_DRAWS_PER_LAYER) return;
  g_r.cmds[layer][*count] = (BrushDrawCmd){.model = NULL,
                                           .mesh = mesh,
                                           .material = material,
                                           .transform = transform,
                                           .tint = WHITE};
  (*count)++;
}

// Draw a submitted command: the submitted matrix becomes the model transform
// for exactly this draw (the model's own transform is saved/restored). Mesh
// commands (model == NULL) draw a raw mesh+material directly.
static void DrawCmd(const BrushDrawCmd *cmd) {
  if (cmd->model == NULL) {
    DrawMesh(cmd->mesh, *cmd->material, cmd->transform);
    return;
  }
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

  // SHADOW — depth-only pass from the sun over this frame's casters. Runs
  // before the scene target opens; receivers then sample the map via PCSS.
  int shadowCount = g_r.cmdCount[BRUSH_LAYER_SHADOW];
  // Horizon fade (industry-standard sun/moon rig detail): near-horizontal
  // light stretches every caster into a shimmering sliver across the whole
  // scene, and the sun->moon handover happens at elevation 0 — so shadow
  // strength eases in between ~1 and ~7 degrees of elevation. Below that the
  // depth pass is skipped entirely (shadows would be invisible anyway).
  float elev = g_r.sunDir.y;
  float st = (elev - 0.02f) / (0.12f - 0.02f);
  if (st < 0.0f) st = 0.0f;
  if (st > 1.0f) st = 1.0f;
  float shadowStrength = st * st * (3.0f - 2.0f * st);
  bool shadowsOn = g_r.shadowsEnabled && g_r.shadow.ready &&
                   shadowCount > 0 && shadowStrength > 0.001f;
  float shOff = 0.0f;
  SetShaderValue(g_r.lit, g_r.locShadowEnabled, &shOff, SHADER_UNIFORM_FLOAT);
  if (shadowsOn) {
    // uShadowEnabled stays 0 during the depth pass (casters draw with the lit
    // shader; only depth is kept, so skip the PCSS cost while rendering it).
    BrushShadowBegin(&g_r.shadow, g_r.sunDir, camera.target);
    for (int i = 0; i < shadowCount; i++)
      DrawCmd(&g_r.cmds[BRUSH_LAYER_SHADOW][i]);
    BrushShadowEnd(&g_r.shadow);

    float shOn = 1.0f;
    SetShaderValue(g_r.lit, g_r.locShadowEnabled, &shOn,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValueMatrix(g_r.lit, g_r.locLightVP, g_r.shadow.lightVP);
    SetShaderValue(g_r.lit, g_r.locShadowSoftness, &g_r.shadow.softness,
                   SHADER_UNIFORM_FLOAT);
    float shadowTexel = 1.0f / (float)g_r.shadow.resolution;
    SetShaderValue(g_r.lit, g_r.locShadowTexel, &shadowTexel,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_r.lit, g_r.locShadowMap, &g_r.shadow.slot,
                   SHADER_UNIFORM_INT);
    SetShaderValue(g_r.lit, g_r.locShadowStrength, &shadowStrength,
                   SHADER_UNIFORM_FLOAT);
    BrushShadowBindMap(&g_r.shadow);
  }

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
    BrushSkyDraw(camera, g_r.skySunDir, g_r.moonDir);

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

void BrushRenderToggleShadows(void) {
  g_r.shadowsEnabled = !g_r.shadowsEnabled;
  TraceLog(LOG_INFO, "BRUSH: sun shadows %s",
           g_r.shadowsEnabled ? "ON" : "OFF");
}

bool BrushRenderShadowsEnabled(void) {
  return g_r.shadowsEnabled && g_r.shadow.ready;
}

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
  case BRUSH_VIEW_SHADOW: return "sun shadow";
  default: return "?";
  }
}
