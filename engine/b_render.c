/*******************************************************************************************
 *   b_render.c - Layered render pipeline
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_assets.h"
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
  BrushMaterialProps props; // per-draw material overrides (hasProps gates)
  bool hasProps;
  BrushSplatDraw splat; // terrain layer blending (hasSplat gates)
  bool hasSplat;
} BrushDrawCmd;

typedef struct BrushRenderState {
  float renderScale; // kept for window-resize re-init
  Shader lit;
  int locSunDir;
  int locSunColor;
  int locAmbient;
  int locLayerView;
  int locSpecStrength;
  int locLinearize;
  int locTriplanar, locTexScale, locHasNormalMap, locNormalDepth;
  int locNormalSwizzled;
  int locHasHeightMap, locHeightScale, locParallax, locParallaxShadow;
  int pomQuality; // 0 = off, 1 = POM, 2 = POM + self-shadow (BRUSH_POM)
  int locHasAoMap, locAoStrength;
  int locSplatEnabled, locSplatOrigin, locSplatSize, locSplatRes;
  int locLayerTiles, locLayerSwizzled, locLayerCount, locAutoSlope;
  int locLayerHeightOn, locLayerHeightStart, locLayerHeightFull;
  int locPomLayer, locPomTile, locPomScale;
  int locHeightBlendLayer, locHeightBlendSharp;
  int locRoadEnabled, locRoadTile;
  int locRoadPom, locRoadPomScale, locRoadHeightBlend, locRoadBlendSharp;
  float specDefault; // uSpecStrength for draws without material props
  float terrainSpec; // uSpecStrength for terrain splat draws (near-matte)
  int locPointPos, locPointColor, locPointRadius, locPointCount;
  int locLightVP[BRUSH_SHADOW_CASCADES];
  int locShadowMap[BRUSH_SHADOW_CASCADES];
  int locCascadeFar;
  int locShadowEnabled;
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
  bool editorMode;

  // Custom scene-draw hook + per-frame lighting snapshot for the bridge
  // (BrushRenderApplySceneLighting reads these; set at the top of Execute).
  void (*sceneDrawCb)(void *user, Camera3D camera);
  void *sceneDrawUser;
  Vector3 curCamPos;
  float curLinearize;
  float curShadowStrength;
  bool curShadowsOn;

  BrushDrawCmd cmds[BRUSH_LAYER_COUNT][BRUSH_MAX_DRAWS_PER_LAYER];
  int cmdCount[BRUSH_LAYER_COUNT];

  // Point lights submitted this frame (cleared after execute). More slots
  // than the shader takes: the nearest BRUSH_MAX_POINT_LIGHTS win.
  BrushPointLight pointLights[64];
  int pointLightCount;
} BrushRenderState;

static BrushRenderState g_r = {0};

void BrushRenderInit(int width, int height, float renderScale) {
  g_r.lit = LoadShader(BrushEnginePath("engine/shaders/lit.vs"), BrushEnginePath("engine/shaders/lit.fs"));
  g_r.lit.locs[SHADER_LOC_VECTOR_VIEW] =
      GetShaderLocation(g_r.lit, "viewPos");
  g_r.locSunDir = GetShaderLocation(g_r.lit, "uSunDir");
  g_r.locSunColor = GetShaderLocation(g_r.lit, "uSunColor");
  g_r.locAmbient = GetShaderLocation(g_r.lit, "uAmbient");
  g_r.locLayerView = GetShaderLocation(g_r.lit, "uLayerView");
  g_r.locSpecStrength = GetShaderLocation(g_r.lit, "uSpecStrength");
  g_r.locLinearize = GetShaderLocation(g_r.lit, "uLinearize");
  g_r.locTriplanar = GetShaderLocation(g_r.lit, "uTriplanar");
  g_r.locTexScale = GetShaderLocation(g_r.lit, "uTexScale");
  g_r.locHasNormalMap = GetShaderLocation(g_r.lit, "uHasNormalMap");
  g_r.locNormalDepth = GetShaderLocation(g_r.lit, "uNormalDepth");
  g_r.locNormalSwizzled = GetShaderLocation(g_r.lit, "uNormalSwizzled");
  // raylib binds MATERIAL_MAP_NORMAL to whatever loc this maps to; without
  // it, per-draw normal maps would never reach the shader.
  g_r.lit.locs[SHADER_LOC_MAP_NORMAL] = GetShaderLocation(g_r.lit, "texture2");
  g_r.lit.locs[SHADER_LOC_MAP_OCCLUSION] = GetShaderLocation(g_r.lit, "texture4");
  g_r.lit.locs[SHADER_LOC_MAP_HEIGHT] = GetShaderLocation(g_r.lit, "texture6");
  g_r.locHasHeightMap = GetShaderLocation(g_r.lit, "uHasHeightMap");
  g_r.locHeightScale = GetShaderLocation(g_r.lit, "uHeightScale");
  g_r.locParallax = GetShaderLocation(g_r.lit, "uParallax");
  g_r.locParallaxShadow = GetShaderLocation(g_r.lit, "uParallaxShadow");
  {
    const char *pq = getenv("BRUSH_POM"); // 0 off / 1 on / 2 on+self-shadow
    g_r.pomQuality = pq ? atoi(pq) : 2;
  }
  g_r.locHasAoMap = GetShaderLocation(g_r.lit, "uHasAoMap");
  g_r.locAoStrength = GetShaderLocation(g_r.lit, "uAoStrength");
  g_r.locSplatEnabled = GetShaderLocation(g_r.lit, "uSplatEnabled");
  g_r.locSplatOrigin = GetShaderLocation(g_r.lit, "uSplatOrigin");
  g_r.locSplatSize = GetShaderLocation(g_r.lit, "uSplatSize");
  g_r.locSplatRes = GetShaderLocation(g_r.lit, "uSplatRes");
  g_r.locLayerTiles = GetShaderLocation(g_r.lit, "uLayerTiles");
  g_r.locLayerSwizzled = GetShaderLocation(g_r.lit, "uLayerSwizzled");
  g_r.locLayerCount = GetShaderLocation(g_r.lit, "uLayerCount");
  g_r.locAutoSlope = GetShaderLocation(g_r.lit, "uAutoSlope");
  g_r.locLayerHeightOn = GetShaderLocation(g_r.lit, "uLayerHeightOn");
  g_r.locLayerHeightStart = GetShaderLocation(g_r.lit, "uLayerHeightStart");
  g_r.locLayerHeightFull = GetShaderLocation(g_r.lit, "uLayerHeightFull");
  g_r.locPomLayer = GetShaderLocation(g_r.lit, "uPomLayer");
  g_r.locPomTile = GetShaderLocation(g_r.lit, "uPomTile");
  g_r.locPomScale = GetShaderLocation(g_r.lit, "uPomScale");
  g_r.locHeightBlendLayer = GetShaderLocation(g_r.lit, "uHeightBlendLayer");
  g_r.locHeightBlendSharp = GetShaderLocation(g_r.lit, "uHeightBlendSharp");
  // Splat samplers live on FIXED texture units chosen to dodge raylib's
  // material binds (0=diffuse, 2=normal) and the shadow cascades (10..12):
  // splat=1, layer albedos 1..3 = 3,4,5, layer normals 1..3 = 6,7,8.
  // Layer 0's albedo/normal ride the material's own diffuse/normal maps.
  {
    int u;
    u = 1; SetShaderValue(g_r.lit, GetShaderLocation(g_r.lit, "uSplatMap"), &u, SHADER_UNIFORM_INT);
    u = 3; SetShaderValue(g_r.lit, GetShaderLocation(g_r.lit, "uLayerAlbedo1"), &u, SHADER_UNIFORM_INT);
    u = 4; SetShaderValue(g_r.lit, GetShaderLocation(g_r.lit, "uLayerAlbedo2"), &u, SHADER_UNIFORM_INT);
    u = 5; SetShaderValue(g_r.lit, GetShaderLocation(g_r.lit, "uLayerAlbedo3"), &u, SHADER_UNIFORM_INT);
    u = 6; SetShaderValue(g_r.lit, GetShaderLocation(g_r.lit, "uLayerNormal1"), &u, SHADER_UNIFORM_INT);
    u = 7; SetShaderValue(g_r.lit, GetShaderLocation(g_r.lit, "uLayerNormal2"), &u, SHADER_UNIFORM_INT);
    // (layer-3 normal map dropped to free a sampler unit for the road albedo;
    //  the 4th terrain layer keeps its albedo but shades with the geo normal.)
    u = 9; SetShaderValue(g_r.lit, GetShaderLocation(g_r.lit, "uLayerHeight"), &u, SHADER_UNIFORM_INT);
    // Road surface (composited over the terrain blend): mask + own albedo/normal
    // on the high units (13..15) past the shadow cascades (10..12).
    u = 13; SetShaderValue(g_r.lit, GetShaderLocation(g_r.lit, "uRoadMask"), &u, SHADER_UNIFORM_INT);
    u = 14; SetShaderValue(g_r.lit, GetShaderLocation(g_r.lit, "uRoadAlbedo"), &u, SHADER_UNIFORM_INT);
  }
  g_r.locRoadEnabled = GetShaderLocation(g_r.lit, "uRoadEnabled");
  g_r.locRoadTile = GetShaderLocation(g_r.lit, "uRoadTile");
  g_r.locRoadPom = GetShaderLocation(g_r.lit, "uRoadPom");
  g_r.locRoadPomScale = GetShaderLocation(g_r.lit, "uRoadPomScale");
  g_r.locRoadHeightBlend = GetShaderLocation(g_r.lit, "uRoadHeightBlend");
  g_r.locRoadBlendSharp = GetShaderLocation(g_r.lit, "uRoadBlendSharp");
  g_r.locPointPos = GetShaderLocation(g_r.lit, "uPointPos");
  g_r.locPointColor = GetShaderLocation(g_r.lit, "uPointColor");
  g_r.locPointRadius = GetShaderLocation(g_r.lit, "uPointRadius");
  g_r.locPointCount = GetShaderLocation(g_r.lit, "uPointCount");
  g_r.locLightVP[0] = GetShaderLocation(g_r.lit, "lightVP0");
  g_r.locLightVP[1] = GetShaderLocation(g_r.lit, "lightVP1");
  g_r.locLightVP[2] = GetShaderLocation(g_r.lit, "lightVP2");
  g_r.locShadowMap[0] = GetShaderLocation(g_r.lit, "shadowMap0");
  g_r.locShadowMap[1] = GetShaderLocation(g_r.lit, "shadowMap1");
  g_r.locShadowMap[2] = GetShaderLocation(g_r.lit, "shadowMap2");
  g_r.locCascadeFar = GetShaderLocation(g_r.lit, "uCascadeFar");
  g_r.locShadowEnabled = GetShaderLocation(g_r.lit, "uShadowEnabled");
  g_r.locShadowSoftness = GetShaderLocation(g_r.lit, "uShadowSoftness");
  g_r.locShadowTexel = GetShaderLocation(g_r.lit, "uShadowTexel");
  g_r.locShadowStrength = GetShaderLocation(g_r.lit, "uShadowStrength");

  // Pleasant default morning sun; games override via BrushSetSun or drive it
  // from a clock with BrushRenderApplyTimeOfDay.
  BrushSetSun((Vector3){0.45f, 0.55f, 0.35f}, (Vector3){1.0f, 0.96f, 0.88f},
              (Vector3){0.36f, 0.38f, 0.42f});
  g_r.moonDir = (Vector3){0.0f, -1.0f, 0.0f};

  g_r.specDefault = 0.35f;
  SetShaderValue(g_r.lit, g_r.locSpecStrength, &g_r.specDefault,
                 SHADER_UNIFORM_FLOAT);
  // Terrain (grass/dirt/rock) is rough/matte — the 0.35 prop default gives it
  // a plasticky specular sheen that whitens sun-facing ground (reads as washed
  // out / "translucent" once tonemapped). Near-matte by default; env-tunable.
  {
    const char *ts = getenv("BRUSH_TERRAIN_SPEC");
    g_r.terrainSpec = ts ? (float)atof(ts) : 0.04f;
  }

  g_r.skyEnabled = true;
  g_r.layerView = BRUSH_VIEW_FINAL;
  BrushSkyInit();

  g_r.renderScale = renderScale;
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
  BrushRenderSubmitEx(layer, model, transform, tint, NULL);
}

void BrushRenderSubmitEx(BrushLayer layer, Model *model, Matrix transform,
                         Color tint, const BrushMaterialProps *props) {
  if (layer < 0 || layer >= BRUSH_LAYER_COUNT) return;
  int *count = &g_r.cmdCount[layer];
  if (*count >= BRUSH_MAX_DRAWS_PER_LAYER) return;
  BrushDrawCmd *cmd = &g_r.cmds[layer][*count];
  *cmd = (BrushDrawCmd){.model = model, .transform = transform, .tint = tint};
  if (props != NULL) {
    cmd->props = *props;
    cmd->hasProps = true;
  }
  (*count)++;
}

void BrushRenderSubmitPointLight(BrushPointLight light) {
  int cap = (int)(sizeof(g_r.pointLights) / sizeof(g_r.pointLights[0]));
  if (g_r.pointLightCount >= cap) return;
  g_r.pointLights[g_r.pointLightCount++] = light;
}

// Upload the nearest BRUSH_MAX_POINT_LIGHTS to the lit shader.
static void UploadPointLights(Vector3 camPos) {
  int n = g_r.pointLightCount;
  BrushPointLight *l = g_r.pointLights;
  if (n > BRUSH_MAX_POINT_LIGHTS) {
    // Partial selection sort: pull the nearest MAX to the front. n is tiny.
    for (int i = 0; i < BRUSH_MAX_POINT_LIGHTS; i++) {
      int best = i;
      float bestD = Vector3DistanceSqr(l[i].position, camPos);
      for (int j = i + 1; j < n; j++) {
        float d = Vector3DistanceSqr(l[j].position, camPos);
        if (d < bestD) { best = j; bestD = d; }
      }
      BrushPointLight tmp = l[i]; l[i] = l[best]; l[best] = tmp;
    }
    n = BRUSH_MAX_POINT_LIGHTS;
  }

  float pos[BRUSH_MAX_POINT_LIGHTS * 3] = {0};
  float col[BRUSH_MAX_POINT_LIGHTS * 3] = {0};
  float rad[BRUSH_MAX_POINT_LIGHTS] = {0};
  for (int i = 0; i < n; i++) {
    pos[i * 3 + 0] = l[i].position.x;
    pos[i * 3 + 1] = l[i].position.y;
    pos[i * 3 + 2] = l[i].position.z;
    col[i * 3 + 0] = l[i].color.x;
    col[i * 3 + 1] = l[i].color.y;
    col[i * 3 + 2] = l[i].color.z;
    rad[i] = (l[i].radius > 0.01f) ? l[i].radius : 0.01f;
  }
  SetShaderValueV(g_r.lit, g_r.locPointPos, pos, SHADER_UNIFORM_VEC3,
                  BRUSH_MAX_POINT_LIGHTS);
  SetShaderValueV(g_r.lit, g_r.locPointColor, col, SHADER_UNIFORM_VEC3,
                  BRUSH_MAX_POINT_LIGHTS);
  SetShaderValueV(g_r.lit, g_r.locPointRadius, rad, SHADER_UNIFORM_FLOAT,
                  BRUSH_MAX_POINT_LIGHTS);
  SetShaderValue(g_r.lit, g_r.locPointCount, &n, SHADER_UNIFORM_INT);
}

void BrushRenderSubmitMesh(BrushLayer layer, Mesh mesh, Material *material,
                           Matrix transform) {
  BrushRenderSubmitMeshSplat(layer, mesh, material, transform, NULL);
}

void BrushRenderSubmitMeshSplat(BrushLayer layer, Mesh mesh,
                                Material *material, Matrix transform,
                                const BrushSplatDraw *splat) {
  if (layer < 0 || layer >= BRUSH_LAYER_COUNT) return;
  int *count = &g_r.cmdCount[layer];
  if (*count >= BRUSH_MAX_DRAWS_PER_LAYER) return;
  BrushDrawCmd *cmd = &g_r.cmds[layer][*count];
  *cmd = (BrushDrawCmd){.model = NULL,
                        .mesh = mesh,
                        .material = material,
                        .transform = transform,
                        .tint = WHITE};
  if (splat != NULL && splat->layerCount > 0 && splat->splat.id != 0) {
    cmd->splat = *splat;
    cmd->hasSplat = true;
  }
  (*count)++;
}

// Per-draw material uniforms. Every lit draw passes through here so a
// textured block never leaks its triplanar/normal-map state into the plain
// draw that follows it.
static void ApplyMaterialProps(const BrushDrawCmd *cmd) {
  const BrushMaterialProps *p = cmd->hasProps ? &cmd->props : NULL;
  float triplanar = (p && p->triplanar) ? 1.0f : 0.0f;
  float texScale = (p && p->texScale > 0.001f) ? p->texScale : 1.0f;
  float hasNormal = (p && p->normal.id != 0) ? 1.0f : 0.0f;
  float normalDepth = p ? p->normalDepth : 1.0f;
  float normalSwizzled = (p && p->normalSwizzled) ? 1.0f : 0.0f;
  float spec = (p && p->specStrength >= 0.0f) ? p->specStrength
                                              : g_r.specDefault;
  float hasHeight = (p && p->displacement.id != 0) ? 1.0f : 0.0f;
  float heightScale = p ? p->heightScale : 0.05f;
  float parallax = (p && p->parallax && g_r.pomQuality > 0) ? 1.0f : 0.0f;
  float pomShadow = (g_r.pomQuality >= 2) ? 1.0f : 0.0f;
  float hasAo = (p && p->ao.id != 0) ? 1.0f : 0.0f;
  float aoStrength = p ? p->aoStrength : 1.0f;

  SetShaderValue(g_r.lit, g_r.locTriplanar, &triplanar, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locTexScale, &texScale, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locHasNormalMap, &hasNormal,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locNormalDepth, &normalDepth,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locNormalSwizzled, &normalSwizzled,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locSpecStrength, &spec, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locHasHeightMap, &hasHeight, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locHeightScale, &heightScale, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locParallax, &parallax, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locParallaxShadow, &pomShadow, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locHasAoMap, &hasAo, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locAoStrength, &aoStrength, SHADER_UNIFORM_FLOAT);
}

// Draw a submitted command: the submitted matrix becomes the model transform
// for exactly this draw (the model's own transform is saved/restored). Mesh
// commands (model == NULL) draw a raw mesh+material directly. Material props
// temporarily retarget the model's texture maps (shared unit-cube pattern).
// Per-draw material props retarget the texture maps of EVERY material on
// the model, not just index 0: glTF loads prepend raylib's default material
// at 0, so real meshes reference materials 1..N — overriding only [0] would
// silently do nothing for them.
#define DRAWCMD_MAX_MATERIALS 8

// Bind the splat weight texture + layer sets to their fixed units and point
// the material's diffuse/normal at layer 0; uSplatEnabled gates the shader
// branch and is reset after the draw so the state never leaks.
static void ApplySplat(const BrushDrawCmd *cmd, Material *mat,
                       Texture2D *savedAlbedo, Texture2D *savedNormal) {
  const BrushSplatDraw *sp = &cmd->splat;
  *savedAlbedo = mat->maps[MATERIAL_MAP_DIFFUSE].texture;
  *savedNormal = mat->maps[MATERIAL_MAP_NORMAL].texture;
  if (sp->layers[0].albedo.id != 0)
    mat->maps[MATERIAL_MAP_DIFFUSE].texture = sp->layers[0].albedo;
  mat->maps[MATERIAL_MAP_NORMAL].texture = sp->layers[0].normal;

  const int albedoUnit[4] = {0, 3, 4, 5}, normalUnit[4] = {0, 6, 7, 8};
  rlActiveTextureSlot(1);
  rlEnableTexture(sp->splat.id);
  for (int i = 1; i < sp->layerCount && i < BRUSH_TERRAIN_LAYERS; i++) {
    if (sp->layers[i].albedo.id != 0) {
      rlActiveTextureSlot(albedoUnit[i]);
      rlEnableTexture(sp->layers[i].albedo.id);
    }
    if (sp->layers[i].normal.id != 0) {
      rlActiveTextureSlot(normalUnit[i]);
      rlEnableTexture(sp->layers[i].normal.id);
    }
  }
  // ONE "special" layer (first with a displacement map that wants POM and/or
  // height-blend, e.g. paving) binds its height to the one free unit (9); its
  // height then drives both POM and the height-based edge blend.
  int specialLayer = -1;
  for (int i = 0; i < sp->layerCount && i < BRUSH_TERRAIN_LAYERS; i++)
    if (sp->layers[i].displacement.id != 0 &&
        (sp->layers[i].parallax || sp->layers[i].heightBlend)) {
      specialLayer = i;
      break;
    }
  // Road surface: coverage mask + own albedo on units 13..14.
  bool road = sp->hasRoad && sp->roadMask.id != 0 && sp->roadLayer.albedo.id != 0;
  // The road's displacement, if any, takes the single POM height unit (9) — the
  // paving use case moved from a terrain layer to the road — and disables
  // terrain-layer POM for this draw (they share the one unit).
  bool roadPom = road && sp->roadLayer.displacement.id != 0 &&
                 (sp->roadLayer.parallax || sp->roadLayer.heightBlend);
  if (roadPom) {
    rlActiveTextureSlot(9);
    rlEnableTexture(sp->roadLayer.displacement.id);
    specialLayer = -1; // unit 9 now holds the ROAD height, not a terrain layer
  } else if (specialLayer >= 0) {
    rlActiveTextureSlot(9);
    rlEnableTexture(sp->layers[specialLayer].displacement.id);
  }
  if (road) {
    rlActiveTextureSlot(13); rlEnableTexture(sp->roadMask.id);
    rlActiveTextureSlot(14); rlEnableTexture(sp->roadLayer.albedo.id);
  }
  rlActiveTextureSlot(0);
  int pomLayer = (specialLayer >= 0 && g_r.pomQuality > 0 &&
                  sp->layers[specialLayer].parallax) ? specialLayer : -1;
  int hbLayer = (specialLayer >= 0 && sp->layers[specialLayer].heightBlend)
                    ? specialLayer : -1;

  float on = 1.0f;
  float origin[2] = {sp->origin.x, sp->origin.z};
  float size = sp->size;
  float res = (float)sp->res;
  float tiles[4], swiz[4];
  float hasNrm = (sp->layers[0].normal.id != 0) ? 1.0f : 0.0f;
  for (int i = 0; i < 4; i++) {
    tiles[i] = (i < sp->layerCount && sp->layers[i].tile > 0.01f)
                   ? sp->layers[i].tile : 1.0f;
    swiz[i] = (i < sp->layerCount && sp->layers[i].normalSwizzled) ? 1.0f : 0.0f;
  }
  int lc = sp->layerCount;
  float autoSlope[3] = {
      (float)((sp->autoSlopeLayer >= 0 && sp->autoSlopeLayer < lc)
                  ? sp->autoSlopeLayer : -1),
      cosf(sp->autoSlopeStart * DEG2RAD),
      cosf(sp->autoSlopeEnd * DEG2RAD)};
  SetShaderValue(g_r.lit, g_r.locAutoSlope, autoSlope, SHADER_UNIFORM_VEC3);
  float hOn[4] = {0}, hStart[4] = {0}, hFull[4] = {0};
  for (int i = 0; i < 4 && i < BRUSH_TERRAIN_LAYERS; i++) {
    hOn[i] = (i < lc && sp->layerHeightOn[i]) ? 1.0f : 0.0f;
    hStart[i] = sp->layerHeightStart[i];
    hFull[i] = sp->layerHeightFull[i];
  }
  SetShaderValue(g_r.lit, g_r.locLayerHeightOn, hOn, SHADER_UNIFORM_VEC4);
  SetShaderValue(g_r.lit, g_r.locLayerHeightStart, hStart, SHADER_UNIFORM_VEC4);
  SetShaderValue(g_r.lit, g_r.locLayerHeightFull, hFull, SHADER_UNIFORM_VEC4);
  SetShaderValue(g_r.lit, g_r.locSplatEnabled, &on, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locSplatOrigin, origin, SHADER_UNIFORM_VEC2);
  SetShaderValue(g_r.lit, g_r.locSplatSize, &size, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locSplatRes, &res, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locLayerTiles, tiles, SHADER_UNIFORM_VEC4);
  SetShaderValue(g_r.lit, g_r.locLayerSwizzled, swiz, SHADER_UNIFORM_VEC4);
  SetShaderValue(g_r.lit, g_r.locLayerCount, &lc, SHADER_UNIFORM_INT);
  SetShaderValue(g_r.lit, g_r.locHasNormalMap, &hasNrm, SHADER_UNIFORM_FLOAT);
  // The bound height (unit 9) belongs to specialLayer; its tile drives both POM
  // and the height-blend sampling.
  float pomTile = specialLayer >= 0 ? tiles[specialLayer] : 1.0f;
  float pomScale = specialLayer >= 0 ? sp->layers[specialLayer].heightScale : 0.05f;
  float hbSharp = hbLayer >= 0 ? sp->layers[hbLayer].blendSharp : 0.2f;
  SetShaderValue(g_r.lit, g_r.locPomLayer, &pomLayer, SHADER_UNIFORM_INT);
  SetShaderValue(g_r.lit, g_r.locPomTile, &pomTile, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locPomScale, &pomScale, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locHeightBlendLayer, &hbLayer, SHADER_UNIFORM_INT);
  SetShaderValue(g_r.lit, g_r.locHeightBlendSharp, &hbSharp, SHADER_UNIFORM_FLOAT);
  float pomSh = (g_r.pomQuality >= 2) ? 1.0f : 0.0f;
  SetShaderValue(g_r.lit, g_r.locParallaxShadow, &pomSh, SHADER_UNIFORM_FLOAT);
  float roadOn = road ? 1.0f : 0.0f;
  float roadTile = (sp->roadLayer.tile > 0.01f) ? sp->roadLayer.tile : 4.0f;
  SetShaderValue(g_r.lit, g_r.locRoadEnabled, &roadOn, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locRoadTile, &roadTile, SHADER_UNIFORM_FLOAT);
  // Road POM (parallax) needs the pom-quality gate; height-blend is independent.
  float roadPomOn = (roadPom && sp->roadLayer.parallax && g_r.pomQuality > 0) ? 1.0f : 0.0f;
  float roadPomScale = (sp->roadLayer.heightScale > 0.0f) ? sp->roadLayer.heightScale : 0.05f;
  float roadHbOn = (roadPom && sp->roadLayer.heightBlend) ? 1.0f : 0.0f;
  float roadHbSharp = (sp->roadLayer.blendSharp > 0.0f) ? sp->roadLayer.blendSharp : 0.2f;
  SetShaderValue(g_r.lit, g_r.locRoadPom, &roadPomOn, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locRoadPomScale, &roadPomScale, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locRoadHeightBlend, &roadHbOn, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_r.lit, g_r.locRoadBlendSharp, &roadHbSharp, SHADER_UNIFORM_FLOAT);
  // Matte terrain: override the prop-default specular ApplyMaterialProps set
  // (runs before this) so grass/dirt don't get a plasticky sun sheen.
  SetShaderValue(g_r.lit, g_r.locSpecStrength, &g_r.terrainSpec,
                 SHADER_UNIFORM_FLOAT);
}

static void DrawCmd(const BrushDrawCmd *cmd) {
  ApplyMaterialProps(cmd);
  if (cmd->model == NULL) {
    if (cmd->hasSplat) {
      Texture2D savedAlbedo, savedNormal;
      ApplySplat(cmd, cmd->material, &savedAlbedo, &savedNormal);
      DrawMesh(cmd->mesh, *cmd->material, cmd->transform);
      cmd->material->maps[MATERIAL_MAP_DIFFUSE].texture = savedAlbedo;
      cmd->material->maps[MATERIAL_MAP_NORMAL].texture = savedNormal;
      float off = 0.0f;
      SetShaderValue(g_r.lit, g_r.locSplatEnabled, &off, SHADER_UNIFORM_FLOAT);
      SetShaderValue(g_r.lit, g_r.locRoadEnabled, &off, SHADER_UNIFORM_FLOAT);
      return;
    }
    DrawMesh(cmd->mesh, *cmd->material, cmd->transform);
    return;
  }
  int matCount = cmd->model->materialCount;
  if (matCount > DRAWCMD_MAX_MATERIALS) matCount = DRAWCMD_MAX_MATERIALS;
  Texture2D savedAlbedo[DRAWCMD_MAX_MATERIALS];
  Texture2D savedNormal[DRAWCMD_MAX_MATERIALS];
  Texture2D savedOcclusion[DRAWCMD_MAX_MATERIALS];
  Texture2D savedHeight[DRAWCMD_MAX_MATERIALS];
  if (cmd->hasProps) {
    for (int i = 0; i < matCount; i++) {
      Material *mat = &cmd->model->materials[i];
      savedAlbedo[i] = mat->maps[MATERIAL_MAP_DIFFUSE].texture;
      savedNormal[i] = mat->maps[MATERIAL_MAP_NORMAL].texture;
      savedOcclusion[i] = mat->maps[MATERIAL_MAP_OCCLUSION].texture;
      savedHeight[i] = mat->maps[MATERIAL_MAP_HEIGHT].texture;
      if (cmd->props.albedo.id != 0)
        mat->maps[MATERIAL_MAP_DIFFUSE].texture = cmd->props.albedo;
      mat->maps[MATERIAL_MAP_NORMAL].texture = cmd->props.normal;
      mat->maps[MATERIAL_MAP_OCCLUSION].texture = cmd->props.ao;
      mat->maps[MATERIAL_MAP_HEIGHT].texture = cmd->props.displacement;
    }
  }
  Matrix saved = cmd->model->transform;
  cmd->model->transform = cmd->transform;
  DrawModel(*cmd->model, (Vector3){0, 0, 0}, 1.0f, cmd->tint);
  cmd->model->transform = saved;
  if (cmd->hasProps) {
    for (int i = 0; i < matCount; i++) {
      Material *mat = &cmd->model->materials[i];
      mat->maps[MATERIAL_MAP_DIFFUSE].texture = savedAlbedo[i];
      mat->maps[MATERIAL_MAP_NORMAL].texture = savedNormal[i];
      mat->maps[MATERIAL_MAP_OCCLUSION].texture = savedOcclusion[i];
      mat->maps[MATERIAL_MAP_HEIGHT].texture = savedHeight[i];
    }
  }
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
  UploadPointLights(camera.position);

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
  g_r.curCamPos = camera.position;
  g_r.curShadowsOn = shadowsOn;
  g_r.curShadowStrength = shadowsOn ? shadowStrength : 0.0f;
  float shOff = 0.0f;
  SetShaderValue(g_r.lit, g_r.locShadowEnabled, &shOff, SHADER_UNIFORM_FLOAT);
  if (shadowsOn) {
    // uShadowEnabled stays 0 during the depth passes (casters draw with the
    // lit shader; only depth is kept, so skip the PCSS cost while rendering).
    // One depth pass per cascade over the same caster list.
    for (int c = 0; c < BRUSH_SHADOW_CASCADES; c++) {
      BrushShadowBeginCascade(&g_r.shadow, c, g_r.sunDir, camera);
      for (int i = 0; i < shadowCount; i++)
        DrawCmd(&g_r.cmds[BRUSH_LAYER_SHADOW][i]);
      BrushShadowEnd(&g_r.shadow);
    }

    float shOn = 1.0f;
    SetShaderValue(g_r.lit, g_r.locShadowEnabled, &shOn,
                   SHADER_UNIFORM_FLOAT);
    for (int c = 0; c < BRUSH_SHADOW_CASCADES; c++) {
      SetShaderValueMatrix(g_r.lit, g_r.locLightVP[c],
                           g_r.shadow.lightVP[c]);
      int slot = g_r.shadow.slot + c;
      SetShaderValue(g_r.lit, g_r.locShadowMap[c], &slot,
                     SHADER_UNIFORM_INT);
    }
    SetShaderValue(g_r.lit, g_r.locCascadeFar, g_r.shadow.splitFar,
                   SHADER_UNIFORM_VEC3);
    SetShaderValue(g_r.lit, g_r.locShadowSoftness, &g_r.shadow.softness,
                   SHADER_UNIFORM_FLOAT);
    float shadowTexel = 1.0f / (float)g_r.shadow.resolution;
    SetShaderValue(g_r.lit, g_r.locShadowTexel, &shadowTexel,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_r.lit, g_r.locShadowStrength, &shadowStrength,
                   SHADER_UNIFORM_FLOAT);
    BrushShadowBindMaps(&g_r.shadow);
  }

  // With post on, the layer stack renders into the linear HDR target and
  // b_post composites to the backbuffer. Debug layer views bypass post so the
  // isolated terms stay readable. With post off (F3), surfaces draw straight
  // to the LDR backbuffer — a diagnostic fallback: linear output looks flat.
  bool usePost = g_r.postEnabled && g_r.post.ready &&
                 g_r.layerView == BRUSH_VIEW_FINAL;
  float linearize = usePost ? 1.0f : 0.0f;
  g_r.curLinearize = linearize;
  SetShaderValue(g_r.lit, g_r.locLinearize, &linearize, SHADER_UNIFORM_FLOAT);
  if (usePost) BrushPostBeginScene(&g_r.post);

  BeginMode3D(camera);

  // Capture the exact matrices raylib set for this scene so depth-based
  // passes (SSAO) reconstruct positions that match the rasterization.
  if (usePost) {
    g_r.post.projectionMatrix = rlGetMatrixProjection();
    g_r.post.viewMatrix = rlGetMatrixModelview();
  }

  // OPAQUE — the color pass (albedo * (ambient + diffuse) + specular).
  for (int i = 0; i < g_r.cmdCount[BRUSH_LAYER_OPAQUE]; i++)
    DrawCmd(&g_r.cmds[BRUSH_LAYER_OPAQUE][i]);

  // Custom opaque geometry (instanced foliage) — same depth buffer as terrain.
  if (g_r.sceneDrawCb) g_r.sceneDrawCb(g_r.sceneDrawUser, camera);

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
    // Scene state for the depth/volumetric passes (DOF focus, god rays,
    // ground fog) — same handoff pattern as the SSAO camera matrices.
    g_r.post.cameraPos = camera.position;
    g_r.post.focusDist = Vector3Distance(camera.position, camera.target);
    g_r.post.sunDir = g_r.sunDir;
    g_r.post.sunColor = g_r.sunColor;
    g_r.post.ambientColor = g_r.ambient;
    // God rays march up to ~100 m — the FAR cascade covers that range.
    g_r.post.lightVP = g_r.shadow.lightVP[BRUSH_SHADOW_CASCADES - 1];
    g_r.post.shadowMap =
        shadowsOn ? g_r.shadow.map[BRUSH_SHADOW_CASCADES - 1].depth
                  : (Texture2D){0};
    if (g_r.editorMode) {
      BrushPostRunNoPresent(&g_r.post, (float)GetTime());
    } else {
      BrushPostRun(&g_r.post, (float)GetTime());
    }
  }

  for (int i = 0; i < BRUSH_LAYER_COUNT; i++) g_r.cmdCount[i] = 0;
  g_r.pointLightCount = 0; // lights are per-frame submissions, like draws
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

struct BrushShadow *BrushRenderGetShadow(void) {
  return g_r.shadow.ready ? &g_r.shadow : NULL;
}

void BrushRenderSetEditorMode(bool enabled) {
  g_r.editorMode = enabled;
}

void BrushRenderResize(int width, int height) {
  if (width <= 0 || height <= 0) return;
  if (width == g_r.post.outW && height == g_r.post.outH) return;
  // The post pipeline owns every screen-sized target; rebuild it wholesale.
  // Cheap enough for interactive window resizing (a few FBOs).
  BrushPostUnload(&g_r.post);
  BrushPostInit(&g_r.post, width, height, g_r.renderScale);
}

void BrushRenderCycleLayerView(void) {
  g_r.layerView = (BrushLayerView)((g_r.layerView + 1) % BRUSH_VIEW_COUNT);
  TraceLog(LOG_INFO, "BRUSH: layer view -> %s", BrushRenderLayerViewName());
}

void BrushRenderSetPomQuality(int quality) {
  g_r.pomQuality = quality < 0 ? 0 : (quality > 2 ? 2 : quality);
}
int BrushRenderGetPomQuality(void) { return g_r.pomQuality; }

void BrushRenderSetSceneCallback(void (*cb)(void *user, Camera3D camera),
                                 void *user) {
  g_r.sceneDrawCb = cb;
  g_r.sceneDrawUser = user;
}

void BrushRenderApplySceneLighting(Shader s) {
  SetShaderValue(s, GetShaderLocation(s, "uSunDir"), &g_r.sunDir, SHADER_UNIFORM_VEC3);
  SetShaderValue(s, GetShaderLocation(s, "uSunColor"), &g_r.sunColor, SHADER_UNIFORM_VEC3);
  SetShaderValue(s, GetShaderLocation(s, "uAmbient"), &g_r.ambient, SHADER_UNIFORM_VEC3);
  float vp[3] = {g_r.curCamPos.x, g_r.curCamPos.y, g_r.curCamPos.z};
  SetShaderValue(s, GetShaderLocation(s, "viewPos"), vp, SHADER_UNIFORM_VEC3);
  SetShaderValue(s, GetShaderLocation(s, "uLinearize"), &g_r.curLinearize, SHADER_UNIFORM_FLOAT);

  // CSM: same cascade matrices + shadow-map texture slots the lit shader uses
  // this frame (the depth maps are already bound to those slots by Execute).
  float shEnabled = g_r.curShadowsOn ? 1.0f : 0.0f;
  SetShaderValue(s, GetShaderLocation(s, "uShadowEnabled"), &shEnabled, SHADER_UNIFORM_FLOAT);
  if (g_r.curShadowsOn) {
    const char *vpNames[3] = {"lightVP0", "lightVP1", "lightVP2"};
    const char *smNames[3] = {"shadowMap0", "shadowMap1", "shadowMap2"};
    for (int c = 0; c < BRUSH_SHADOW_CASCADES; c++) {
      SetShaderValueMatrix(s, GetShaderLocation(s, vpNames[c]), g_r.shadow.lightVP[c]);
      int slot = g_r.shadow.slot + c;
      SetShaderValue(s, GetShaderLocation(s, smNames[c]), &slot, SHADER_UNIFORM_INT);
    }
    SetShaderValue(s, GetShaderLocation(s, "uCascadeFar"), g_r.shadow.splitFar, SHADER_UNIFORM_VEC3);
    SetShaderValue(s, GetShaderLocation(s, "uShadowStrength"), &g_r.curShadowStrength, SHADER_UNIFORM_FLOAT);
  }
}

BrushLayerView BrushRenderGetLayerView(void) { return g_r.layerView; }

// --- Frustum culling ----------------------------------------------------------

static Vector4 NormalizePlane(float a, float b, float c, float d) {
  float len = sqrtf(a * a + b * b + c * c);
  if (len < 1e-8f) len = 1.0f;
  return (Vector4){a / len, b / len, c / len, d / len};
}

BrushFrustum BrushRenderMakeFrustum(Camera3D camera) {
  // Mirror BeginMode3D exactly: screen aspect, camera fovy, rl cull near/far.
  float aspect = (float)GetScreenWidth() / (float)GetScreenHeight();
  Matrix view = GetCameraMatrix(camera);
  Matrix proj = MatrixPerspective(camera.fovy * DEG2RAD, aspect,
                                  rlGetCullDistanceNear(),
                                  rlGetCullDistanceFar());
  // clip = Vector3Transform(worldPos, m): clip.x uses (m0,m4,m8,m12),
  // clip.w uses (m3,m7,m11,m15). Gribb-Hartmann plane extraction on those.
  Matrix m = MatrixMultiply(view, proj);
  BrushFrustum f;
  f.planes[0] = NormalizePlane(m.m0 + m.m3, m.m4 + m.m7, m.m8 + m.m11, m.m12 + m.m15);  // left
  f.planes[1] = NormalizePlane(m.m3 - m.m0, m.m7 - m.m4, m.m11 - m.m8, m.m15 - m.m12);  // right
  f.planes[2] = NormalizePlane(m.m1 + m.m3, m.m5 + m.m7, m.m9 + m.m11, m.m13 + m.m15);  // bottom
  f.planes[3] = NormalizePlane(m.m3 - m.m1, m.m7 - m.m5, m.m11 - m.m9, m.m15 - m.m13);  // top
  f.planes[4] = NormalizePlane(m.m2 + m.m3, m.m6 + m.m7, m.m10 + m.m11, m.m14 + m.m15); // near
  f.planes[5] = NormalizePlane(m.m3 - m.m2, m.m7 - m.m6, m.m11 - m.m10, m.m15 - m.m14); // far
  return f;
}

bool BrushFrustumContainsSphere(const BrushFrustum *f, Vector3 center,
                                float radius) {
  for (int i = 0; i < 6; i++) {
    const Vector4 *p = &f->planes[i];
    float dist = p->x * center.x + p->y * center.y + p->z * center.z + p->w;
    if (dist < -radius) return false; // fully outside this plane
  }
  return true;
}

void BrushBoundingSphere(BoundingBox local, Matrix transform, Vector3 *center,
                         float *radius) {
  Vector3 corners[8] = {
      {local.min.x, local.min.y, local.min.z},
      {local.max.x, local.min.y, local.min.z},
      {local.min.x, local.max.y, local.min.z},
      {local.max.x, local.max.y, local.min.z},
      {local.min.x, local.min.y, local.max.z},
      {local.max.x, local.min.y, local.max.z},
      {local.min.x, local.max.y, local.max.z},
      {local.max.x, local.max.y, local.max.z},
  };
  Vector3 wmin = {1e30f, 1e30f, 1e30f}, wmax = {-1e30f, -1e30f, -1e30f};
  for (int i = 0; i < 8; i++) {
    corners[i] = Vector3Transform(corners[i], transform);
    wmin = Vector3Min(wmin, corners[i]);
    wmax = Vector3Max(wmax, corners[i]);
  }
  Vector3 c = Vector3Scale(Vector3Add(wmin, wmax), 0.5f);
  float r2 = 0.0f;
  for (int i = 0; i < 8; i++) {
    float d2 = Vector3DistanceSqr(c, corners[i]);
    if (d2 > r2) r2 = d2;
  }
  *center = c;
  *radius = sqrtf(r2);
}

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
