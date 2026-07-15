/*******************************************************************************************
 *   b_post.c - HDR post-processing (see b_post.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_assets.h"
#include "b_post.h"

#include <raymath.h>
#include <rlgl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static float Frand(void) { return (float)rand() / (float)RAND_MAX; }

// Env override for fast look tuning without a rebuild.
static float EnvF(const char *name, float fallback) {
  const char *v = getenv(name);
  return v ? (float)atof(v) : fallback;
}

// Create a floating-point (RGBA16F) render target. raylib's LoadRenderTexture
// is 8-bit only, so the FBO is built by hand. `withDepth` adds a depth TEXTURE
// (sampleable later by SSAO/fog passes; bloom targets don't need it).
static RenderTexture2D LoadHDRTarget(int w, int h, bool withDepth) {
  RenderTexture2D t = {0};
  t.id = rlLoadFramebuffer();
  if (t.id == 0) {
    TraceLog(LOG_WARNING, "BRUSH post: failed to create HDR framebuffer");
    return t;
  }
  rlEnableFramebuffer(t.id);

  t.texture.id =
      rlLoadTexture(NULL, w, h, RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, 1);
  t.texture.width = w;
  t.texture.height = h;
  t.texture.format = RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
  t.texture.mipmaps = 1;
  rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0,
                      RL_ATTACHMENT_TEXTURE2D, 0);

  if (withDepth) {
    t.depth.id = rlLoadTextureDepth(w, h, false);
    t.depth.width = w;
    t.depth.height = h;
    rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH,
                        RL_ATTACHMENT_TEXTURE2D, 0);
  }

  if (!rlFramebufferComplete(t.id)) {
    TraceLog(LOG_WARNING, "BRUSH post: HDR framebuffer incomplete (%dx%d)", w,
             h);
    rlDisableFramebuffer();
    rlUnloadFramebuffer(t.id);
    return (RenderTexture2D){0};
  }
  rlDisableFramebuffer();

  // Bilinear so the low-res bloom mips upsample smoothly.
  SetTextureFilter(t.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(t.texture, TEXTURE_WRAP_CLAMP);
  return t;
}

static void UnloadHDRTarget(RenderTexture2D *t) {
  if (t->id > 0) {
    rlUnloadFramebuffer(t->id);
    if (t->texture.id > 0) rlUnloadTexture(t->texture.id);
    if (t->depth.id > 0) rlUnloadTexture(t->depth.id);
    *t = (RenderTexture2D){0};
  }
}

static void LoadCompositePermutation(BrushPost *pp, int index) {
  bool dof = (index & 1) != 0;
  bool godRays = (index & 2) != 0;
  bool ao = (index & 4) != 0;

  char *baseSrc = LoadFileText(BrushEnginePath("engine/shaders/composite.fs"));
  if (!baseSrc) return;

  const char *header = "#version 330\n"
                       "%s%s%s"
                       "#define COMPILED_PERMUTATION\n";
  char defines[256];
  sprintf(defines, header,
          dof ? "#define ENABLE_DOF\n" : "",
          godRays ? "#define ENABLE_GODRAYS\n" : "",
          ao ? "#define ENABLE_AO\n" : "");

  // Remove the #version 330 from the base source if present
  const char *versionStr = "#version 330\n";
  char *codeStart = baseSrc;
  if (strncmp(baseSrc, versionStr, strlen(versionStr)) == 0) {
    codeStart += strlen(versionStr);
  }

  char *fullSrc = malloc(strlen(defines) + strlen(codeStart) + 1);
  strcpy(fullSrc, defines);
  strcat(fullSrc, codeStart);

  pp->compositePerms[index].shader = LoadShaderFromMemory(NULL, fullSrc);
  Shader s = pp->compositePerms[index].shader;

  pp->compositePerms[index].locBloomTex = GetShaderLocation(s, "uBloom");
  pp->compositePerms[index].locBloomIntensity = GetShaderLocation(s, "uBloomIntensity");
  pp->compositePerms[index].locExposure = GetShaderLocation(s, "uExposure");
  pp->compositePerms[index].locResolution = GetShaderLocation(s, "uResolution");
  pp->compositePerms[index].locTime = GetShaderLocation(s, "uTime");
  pp->compositePerms[index].locDisplayP3 = GetShaderLocation(s, "uDisplayP3");
  pp->compositePerms[index].locP3Strength = GetShaderLocation(s, "uP3Strength");
  pp->compositePerms[index].locAOTex = GetShaderLocation(s, "uAO");
  pp->compositePerms[index].locAOEnabled = GetShaderLocation(s, "uAOEnabled");
  pp->compositePerms[index].locDofDepth = GetShaderLocation(s, "uDepth");
  pp->compositePerms[index].locDofNear = GetShaderLocation(s, "uNear");
  pp->compositePerms[index].locDofFar = GetShaderLocation(s, "uFar");
  pp->compositePerms[index].locDofFocus = GetShaderLocation(s, "uFocusDistance");
  pp->compositePerms[index].locDofRange = GetShaderLocation(s, "uFocusRange");
  pp->compositePerms[index].locDofStrength = GetShaderLocation(s, "uDofStrength");
  pp->compositePerms[index].locDofEnabled = GetShaderLocation(s, "uDofEnabled");
  pp->compositePerms[index].locGodRayTex = GetShaderLocation(s, "uGodRayTex");
  pp->compositePerms[index].locGodRaysOn = GetShaderLocation(s, "uGodRaysOn");

  free(fullSrc);
  UnloadFileText(baseSrc);
}

void BrushPostInit(BrushPost *pp, int width, int height, float renderScale) {
  *pp = (BrushPost){0};
  pp->outW = width;
  pp->outH = height;
  if (renderScale <= 0.0f) renderScale = 1.0f;
  if (renderScale > 1.0f) renderScale = 1.0f;
  if (renderScale < 0.25f) renderScale = 0.25f;
  pp->renderW = (int)((float)width * renderScale);
  pp->renderH = (int)((float)height * renderScale);

  // The scene's HDR sky/sun glow peaks around luminance ~1-2; a threshold
  // below that lets the bright sky actually bloom softly.
  pp->bloomThreshold = EnvF("BRUSH_BLOOM_THRESH", 1.5f);
  pp->bloomIntensity = EnvF("BRUSH_BLOOM_INT", 0.32f);
  pp->blurPasses = 1;
  pp->exposure = EnvF("BRUSH_EXPOSURE", 1.0f);
  pp->sharpenEnabled = (getenv("BRUSH_NO_SHARPEN") == NULL);
  pp->sharpenAmount = EnvF("BRUSH_SHARPEN", 0.10f);
  pp->ssaoEnabled = (getenv("BRUSH_NO_SSAO") == NULL);
  pp->ssaoRadius = EnvF("BRUSH_SSAO_RADIUS", 0.5f);
  // Bias/strength are the donor's TUNED values. The bias operates in the
  // shader's reconstructed view-Z units (not the textbook ~0.025 world-space
  // bias) — dropping it to that caused depth-quantization self-occlusion:
  // visible stripe banding across large tilted surfaces.
  pp->ssaoBias = EnvF("BRUSH_SSAO_BIAS", 1.50f);
  pp->ssaoStrength = EnvF("BRUSH_SSAO_STRENGTH", 0.50f);
  pp->smaaEnabled = (getenv("BRUSH_NO_SMAA") == NULL);
  pp->smaaThreshold = EnvF("BRUSH_SMAA_THRESH", 0.08f);
  // DOF/god-ray defaults are the donor's tuned values (wide focal zone +
  // subtle strength: an atmospheric hint for third-person cameras).
  pp->dofEnabled = (getenv("BRUSH_NO_DOF") == NULL);
  pp->dofRange = EnvF("BRUSH_DOF_RANGE", 120.0f);
  pp->dofStrength = EnvF("BRUSH_DOF_STRENGTH", 0.25f);
  pp->godRaysEnabled = (getenv("BRUSH_NO_GODRAYS") == NULL);
  pp->godRaysDecay = EnvF("BRUSH_GODRAYS_DECAY", 1.0f);
  pp->godRaysDensity = EnvF("BRUSH_GODRAYS_DENSITY", 0.04f);
  pp->godRaysWeight = EnvF("BRUSH_GODRAYS_WEIGHT", 0.85f);
  pp->godRaysExposure = EnvF("BRUSH_GODRAYS_EXP", 0.7f);
  // Volumetric fog is OPT-IN (scene mood, not a pipeline default).
  pp->volFogEnabled = (getenv("BRUSH_VOLFOG") != NULL);
  pp->volFogDensity = EnvF("BRUSH_VOLFOG_DENSITY", 0.12f);
  pp->volFogGroundY = EnvF("BRUSH_VOLFOG_GROUND", 0.0f);
  pp->volFogTopY = EnvF("BRUSH_VOLFOG_TOP", 3.0f);
  pp->volFogCoverage = EnvF("BRUSH_VOLFOG_COVERAGE", 0.44f);
  pp->projectionMatrix = MatrixIdentity();
  pp->viewMatrix = MatrixIdentity();
  pp->lightVP = MatrixIdentity();
  pp->focusDist = 6.0f;
#if defined(__APPLE__)
  pp->displayP3 = (getenv("BRUSH_NO_P3") == NULL) ? 1.0f : 0.0f;
#else
  pp->displayP3 = (getenv("BRUSH_DISPLAY_P3") != NULL) ? 1.0f : 0.0f;
#endif
  pp->p3Strength = EnvF("BRUSH_P3_STRENGTH", 1.0f);

  pp->scene = LoadHDRTarget(pp->renderW, pp->renderH, true);
  pp->bloomA = LoadHDRTarget(pp->renderW / 2, pp->renderH / 2, false);
  pp->bloomC = LoadHDRTarget(pp->renderW / 4, pp->renderH / 4, false);
  pp->bloomE = LoadHDRTarget(pp->renderW / 8, pp->renderH / 8, false);
  // LDR composite at logical res; the heavy composite shader runs at 1x and
  // the (retina) backbuffer only pays for the cheap sharpen blit.
  pp->present = LoadRenderTexture(pp->outW, pp->outH);
  SetTextureFilter(pp->present.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(pp->present.texture, TEXTURE_WRAP_CLAMP);

  // SSAO at half render-res (8-bit is plenty for an occlusion factor).
  pp->aoRaw = LoadRenderTexture(pp->renderW / 2, pp->renderH / 2);
  pp->aoBlur = LoadRenderTexture(pp->renderW / 2, pp->renderH / 2);
  SetTextureFilter(pp->aoBlur.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(pp->aoBlur.texture, TEXTURE_WRAP_CLAMP);

  // God rays at quarter render-res (fillrate), fog at half render-res; both
  // HDR since they add/blend linear light.
  pp->godRayA = LoadHDRTarget(pp->renderW / 4, pp->renderH / 4, false);
  pp->godRayB = LoadHDRTarget(pp->renderW / 4, pp->renderH / 4, false);
  pp->volFogTex = LoadHDRTarget(pp->renderW / 2, pp->renderH / 2, false);

  // SMAA pass targets at logical res (it runs on the LDR present image).
  pp->smaaEdgesTex = LoadRenderTexture(pp->outW, pp->outH);
  pp->smaaBlendTex = LoadRenderTexture(pp->outW, pp->outH);
  pp->presentAA = LoadRenderTexture(pp->outW, pp->outH);
  SetTextureFilter(pp->smaaEdgesTex.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureFilter(pp->smaaBlendTex.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureFilter(pp->presentAA.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(pp->smaaEdgesTex.texture, TEXTURE_WRAP_CLAMP);
  SetTextureWrap(pp->smaaBlendTex.texture, TEXTURE_WRAP_CLAMP);
  SetTextureWrap(pp->presentAA.texture, TEXTURE_WRAP_CLAMP);

  pp->bright = LoadShader(NULL, BrushEnginePath("engine/shaders/bloom_bright.fs"));
  pp->kawaseDown = LoadShader(NULL, BrushEnginePath("engine/shaders/kawase_down.fs"));
  pp->kawaseUp = LoadShader(NULL, BrushEnginePath("engine/shaders/kawase_up.fs"));
  pp->blur = LoadShader(NULL, BrushEnginePath("engine/shaders/blur.fs"));
  for (int i = 0; i < 8; i++) {
    LoadCompositePermutation(pp, i);
  }
  pp->sharpen = LoadShader(NULL, BrushEnginePath("engine/shaders/sharpen.fs"));

  pp->locBrightThreshold = GetShaderLocation(pp->bright, "uThreshold");
  pp->locKawaseDownTexel = GetShaderLocation(pp->kawaseDown, "uTexel");
  pp->locKawaseUpTexel = GetShaderLocation(pp->kawaseUp, "uTexel");
  pp->locBlurDir = GetShaderLocation(pp->blur, "uDir");
  pp->locBlurTexel = GetShaderLocation(pp->blur, "uTexel");
  pp->locSharpTexel = GetShaderLocation(pp->sharpen, "uTexel");
  pp->locSharpAmount = GetShaderLocation(pp->sharpen, "uSharpen");

  // --- SSAO ---
  pp->ssao = LoadShader(NULL, BrushEnginePath("engine/shaders/ssao.fs"));
  pp->ssaoBlur = LoadShader(NULL, BrushEnginePath("engine/shaders/ssao_blur.fs"));
  pp->locSsaoDepth = GetShaderLocation(pp->ssao, "uDepth");
  pp->locSsaoNoise = GetShaderLocation(pp->ssao, "uNoise");
  pp->locSsaoKernel = GetShaderLocation(pp->ssao, "uSamples");
  pp->locSsaoProj = GetShaderLocation(pp->ssao, "uProjection");
  pp->locSsaoInvProj = GetShaderLocation(pp->ssao, "uInvProjection");
  pp->locSsaoNoiseScale = GetShaderLocation(pp->ssao, "uNoiseScale");
  pp->locSsaoRadius = GetShaderLocation(pp->ssao, "uRadius");
  pp->locSsaoBias = GetShaderLocation(pp->ssao, "uBias");
  pp->locSsaoStrength = GetShaderLocation(pp->ssao, "uStrength");
  pp->locSsaoBlurTexel = GetShaderLocation(pp->ssaoBlur, "uTexel");

  // --- DOF (lives in the composite) ---

  // --- God rays ---
  pp->godrays = LoadShader(NULL, BrushEnginePath("engine/shaders/godrays.fs"));
  pp->locGRDepth = GetShaderLocation(pp->godrays, "uDepth");
  pp->locGRShadowMap = GetShaderLocation(pp->godrays, "uShadowMap");
  pp->locGRInvVP = GetShaderLocation(pp->godrays, "uInvViewProj");
  pp->locGRMatLight = GetShaderLocation(pp->godrays, "uMatLight");
  pp->locGRCamPos = GetShaderLocation(pp->godrays, "uCameraPos");
  pp->locGRSunDir = GetShaderLocation(pp->godrays, "uSunDir");
  pp->locGRSunCol = GetShaderLocation(pp->godrays, "uSunColor");
  pp->locGRRes = GetShaderLocation(pp->godrays, "uResolution");
  pp->locGRTime = GetShaderLocation(pp->godrays, "uTime");
  pp->locGRDecay = GetShaderLocation(pp->godrays, "uGodRaysDecay");
  pp->locGRDensity = GetShaderLocation(pp->godrays, "uGodRaysDensity");
  pp->locGRWeight = GetShaderLocation(pp->godrays, "uGodRaysWeight");
  pp->locGRExposure = GetShaderLocation(pp->godrays, "uGodRaysExposure");

  // --- Volumetric ground fog ---
  pp->volFog = LoadShader(NULL, BrushEnginePath("engine/shaders/volfog.fs"));
  pp->locVFDepth = GetShaderLocation(pp->volFog, "uDepth");
  pp->locVFInvVP = GetShaderLocation(pp->volFog, "uInvViewProj");
  pp->locVFCamPos = GetShaderLocation(pp->volFog, "uCameraPos");
  pp->locVFSunDir = GetShaderLocation(pp->volFog, "uSunDir");
  pp->locVFSunCol = GetShaderLocation(pp->volFog, "uSunColor");
  pp->locVFSkyCol = GetShaderLocation(pp->volFog, "uSkyColor");
  pp->locVFTime = GetShaderLocation(pp->volFog, "uTime");
  pp->locVFWind = GetShaderLocation(pp->volFog, "uWindDir");
  pp->locVFDensity = GetShaderLocation(pp->volFog, "uFogDensity");
  pp->locVFGroundY = GetShaderLocation(pp->volFog, "uFogGroundY");
  pp->locVFTopY = GetShaderLocation(pp->volFog, "uFogTopY");
  pp->locVFCoverage = GetShaderLocation(pp->volFog, "uFogCoverage");

  // Hemisphere kernel: cosine-ish samples packed toward the origin so
  // near-surface occlusion dominates (LearnOpenGL-style).
  for (int i = 0; i < BRUSH_SSAO_KERNEL; i++) {
    float x = Frand() * 2.0f - 1.0f;
    float y = Frand() * 2.0f - 1.0f;
    float z = Frand(); // hemisphere: z in [0,1]
    float len = sqrtf(x * x + y * y + z * z);
    if (len < 1e-5f) len = 1.0f;
    float t = (float)i / (float)BRUSH_SSAO_KERNEL;
    float scale = 0.1f + 0.9f * t * t;
    float sc = Frand() * scale / len;
    pp->ssaoKernel[i * 3 + 0] = x * sc;
    pp->ssaoKernel[i * 3 + 1] = y * sc;
    pp->ssaoKernel[i * 3 + 2] = z * sc;
  }
  SetShaderValueV(pp->ssao, pp->locSsaoKernel, pp->ssaoKernel,
                  SHADER_UNIFORM_VEC3, BRUSH_SSAO_KERNEL);

  // 4x4 noise of random rotation vectors, tiled per pixel.
  Color noisePix[16];
  for (int i = 0; i < 16; i++) {
    float nx = Frand() * 2.0f - 1.0f;
    float ny = Frand() * 2.0f - 1.0f;
    noisePix[i] = (Color){(unsigned char)((nx * 0.5f + 0.5f) * 255.0f),
                          (unsigned char)((ny * 0.5f + 0.5f) * 255.0f), 128,
                          255};
  }
  Image noiseImg = {.data = noisePix,
                    .width = 4,
                    .height = 4,
                    .mipmaps = 1,
                    .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
  pp->noise = LoadTextureFromImage(noiseImg);
  SetTextureFilter(pp->noise, TEXTURE_FILTER_POINT);
  SetTextureWrap(pp->noise, TEXTURE_WRAP_REPEAT);

  // --- SMAA 1x ---
  pp->smaaEdges = LoadShader(NULL, BrushEnginePath("engine/shaders/smaa_edges.fs"));
  pp->smaaWeights = LoadShader(NULL, BrushEnginePath("engine/shaders/smaa_weights.fs"));
  pp->smaaBlend = LoadShader(NULL, BrushEnginePath("engine/shaders/smaa_blend.fs"));
  pp->smaaArea = LoadTexture(BrushEnginePath("engine/resources/smaa/AreaTex.png"));
  pp->smaaSearch = LoadTexture(BrushEnginePath("engine/resources/smaa/SearchTex.png"));
  SetTextureFilter(pp->smaaArea, TEXTURE_FILTER_BILINEAR);
  SetTextureFilter(pp->smaaSearch, TEXTURE_FILTER_POINT);
  SetTextureWrap(pp->smaaArea, TEXTURE_WRAP_CLAMP);
  SetTextureWrap(pp->smaaSearch, TEXTURE_WRAP_CLAMP);
  if (pp->smaaArea.id == 0 || pp->smaaSearch.id == 0) {
    TraceLog(LOG_WARNING,
             "BRUSH post: SMAA lookup textures missing (assets/smaa) — SMAA "
             "disabled");
    pp->smaaEnabled = false;
  }
  pp->locEdgesMetrics = GetShaderLocation(pp->smaaEdges, "uMetrics");
  pp->locEdgesThreshold = GetShaderLocation(pp->smaaEdges, "uThreshold");
  pp->locWeightsMetrics = GetShaderLocation(pp->smaaWeights, "uMetrics");
  pp->locWeightsArea = GetShaderLocation(pp->smaaWeights, "uArea");
  pp->locWeightsSearch = GetShaderLocation(pp->smaaWeights, "uSearch");
  pp->locBlendMetrics = GetShaderLocation(pp->smaaBlend, "uMetrics");
  pp->locBlendWeights = GetShaderLocation(pp->smaaBlend, "uBlend");

  pp->ready = (pp->scene.id != 0 && pp->bloomA.id != 0);
  TraceLog(LOG_INFO,
           "BRUSH post: HDR pipeline %s (scene %dx%d, out %dx%d, scale %.2f)",
           pp->ready ? "ready" : "UNAVAILABLE", pp->renderW, pp->renderH,
           pp->outW, pp->outH, renderScale);
}

void BrushPostUnload(BrushPost *pp) {
  UnloadHDRTarget(&pp->scene);
  UnloadHDRTarget(&pp->bloomA);
  UnloadHDRTarget(&pp->bloomC);
  UnloadHDRTarget(&pp->bloomE);
  UnloadRenderTexture(pp->present);
  UnloadRenderTexture(pp->aoRaw);
  UnloadRenderTexture(pp->aoBlur);
  UnloadRenderTexture(pp->smaaEdgesTex);
  UnloadRenderTexture(pp->smaaBlendTex);
  UnloadRenderTexture(pp->presentAA);
  UnloadHDRTarget(&pp->godRayA);
  UnloadHDRTarget(&pp->godRayB);
  UnloadHDRTarget(&pp->volFogTex);
  UnloadTexture(pp->noise);
  UnloadTexture(pp->smaaArea);
  UnloadTexture(pp->smaaSearch);
  UnloadShader(pp->bright);
  UnloadShader(pp->kawaseDown);
  UnloadShader(pp->kawaseUp);
  UnloadShader(pp->blur);
  for (int i = 0; i < 8; i++) {
    UnloadShader(pp->compositePerms[i].shader);
  }
  UnloadShader(pp->sharpen);
  UnloadShader(pp->ssao);
  UnloadShader(pp->ssaoBlur);
  UnloadShader(pp->smaaEdges);
  UnloadShader(pp->smaaWeights);
  UnloadShader(pp->smaaBlend);
  UnloadShader(pp->godrays);
  UnloadShader(pp->volFog);
  *pp = (BrushPost){0};
}

void BrushPostBeginScene(BrushPost *pp) {
  BeginTextureMode(pp->scene);
  ClearBackground(BLACK);
}

void BrushPostEndScene(BrushPost *pp) {
  (void)pp;
  EndTextureMode();
}

// Full-screen blit of a texture into the current target (scales as needed).
static void BlitTexture(Texture2D tex, int srcW, int srcH, int destW,
                        int destH) {
  DrawTexturePro(tex, (Rectangle){0, 0, (float)srcW, (float)srcH},
                 (Rectangle){0, 0, (float)destW, (float)destH}, (Vector2){0, 0},
                 0.0f, WHITE);
}

// Separable gaussian on `a` (result stays in `a`), `b` as scratch.
static void BlurLevel(BrushPost *pp, RenderTexture2D a, RenderTexture2D b,
                      int w, int h, int passes) {
  float texel[2] = {1.0f / (float)w, 1.0f / (float)h};
  for (int i = 0; i < passes; i++) {
    float dirH[2] = {1.0f, 0.0f};
    BeginTextureMode(b);
    BeginShaderMode(pp->blur);
    SetShaderValue(pp->blur, pp->locBlurDir, dirH, SHADER_UNIFORM_VEC2);
    SetShaderValue(pp->blur, pp->locBlurTexel, texel, SHADER_UNIFORM_VEC2);
    BlitTexture(a.texture, w, h, w, h);
    EndShaderMode();
    EndTextureMode();

    float dirV[2] = {0.0f, 1.0f};
    BeginTextureMode(a);
    BeginShaderMode(pp->blur);
    SetShaderValue(pp->blur, pp->locBlurDir, dirV, SHADER_UNIFORM_VEC2);
    SetShaderValue(pp->blur, pp->locBlurTexel, texel, SHADER_UNIFORM_VEC2);
    BlitTexture(b.texture, w, h, w, h);
    EndShaderMode();
    EndTextureMode();
  }
}

static void KawaseDownsample(BrushPost *pp, RenderTexture2D src, RenderTexture2D dest,
                             int srcW, int srcH, int destW, int destH) {
  float texel[2] = {1.0f / (float)destW, 1.0f / (float)destH};
  BeginTextureMode(dest);
  BeginShaderMode(pp->kawaseDown);
  SetShaderValue(pp->kawaseDown, pp->locKawaseDownTexel, texel, SHADER_UNIFORM_VEC2);
  BlitTexture(src.texture, srcW, srcH, destW, destH);
  EndShaderMode();
  EndTextureMode();
}

static void KawaseUpsample(BrushPost *pp, RenderTexture2D src, RenderTexture2D dest,
                           int srcW, int srcH, int destW, int destH, float weight) {
  float texel[2] = {1.0f / (float)srcW, 1.0f / (float)srcH};
  BeginTextureMode(dest);
  BeginBlendMode(BLEND_ADDITIVE);
  BeginShaderMode(pp->kawaseUp);
  SetShaderValue(pp->kawaseUp, pp->locKawaseUpTexel, texel, SHADER_UNIFORM_VEC2);
  unsigned char w = (unsigned char)(weight * 255.0f);
  DrawTexturePro(src.texture, (Rectangle){0, 0, (float)srcW, (float)srcH},
                 (Rectangle){0, 0, (float)destW, (float)destH}, (Vector2){0, 0},
                 0.0f, (Color){255, 255, 255, w});
  EndShaderMode();
  EndBlendMode();
  EndTextureMode();
}

void BrushPostRunNoPresent(BrushPost *pp, float time) {
  if (!pp->ready) return;
  int hw = pp->renderW / 2, hh = pp->renderH / 2;
  int qw = pp->renderW / 4, qh = pp->renderH / 4;
  int ew = pp->renderW / 8, eh = pp->renderH / 8;

  Matrix invViewProj =
      MatrixInvert(MatrixMultiply(pp->viewMatrix, pp->projectionMatrix));
  float camPos[3] = {pp->cameraPos.x, pp->cameraPos.y, pp->cameraPos.z};
  float sunDir[3] = {pp->sunDir.x, pp->sunDir.y, pp->sunDir.z};
  float sunCol[3] = {pp->sunColor.x, pp->sunColor.y, pp->sunColor.z};

  // -1. Volumetric ground fog: raymarch the view ray up to the scene depth
  //     into a half-res target, then alpha-blend over the HDR scene. Runs
  //     FIRST so the fog is part of the scene when SSAO/bloom/god rays
  //     sample it; the march stops at the rasterized surface, so geometry
  //     occludes the fog.
  if (pp->volFogEnabled) {
    // Ambient/sun colours are linear in brush (lit.fs treats them so).
    float skyCol[3] = {pp->ambientColor.x * pp->exposure,
                       pp->ambientColor.y * pp->exposure,
                       pp->ambientColor.z * pp->exposure};
    float wind[2] = {0.82f, 0.40f}; // gentle horizontal drift
    // Keep most of the mist at midday: fade density by at most 40% as the
    // sun climbs (full density at dawn/dusk).
    float dayFade = (pp->sunDir.y - 0.10f) / (0.45f - 0.10f);
    if (dayFade < 0.0f) dayFade = 0.0f;
    if (dayFade > 1.0f) dayFade = 1.0f;
    float mistDensity = pp->volFogDensity * (1.0f - 0.40f * dayFade);

    BeginTextureMode(pp->volFogTex);
    ClearBackground(BLANK);
    BeginShaderMode(pp->volFog);
    SetShaderValueTexture(pp->volFog, pp->locVFDepth, pp->scene.depth);
    SetShaderValueMatrix(pp->volFog, pp->locVFInvVP, invViewProj);
    SetShaderValue(pp->volFog, pp->locVFCamPos, camPos, SHADER_UNIFORM_VEC3);
    SetShaderValue(pp->volFog, pp->locVFSunDir, sunDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(pp->volFog, pp->locVFSunCol, sunCol, SHADER_UNIFORM_VEC3);
    SetShaderValue(pp->volFog, pp->locVFSkyCol, skyCol, SHADER_UNIFORM_VEC3);
    SetShaderValue(pp->volFog, pp->locVFTime, &time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->volFog, pp->locVFWind, wind, SHADER_UNIFORM_VEC2);
    SetShaderValue(pp->volFog, pp->locVFDensity, &mistDensity,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->volFog, pp->locVFGroundY, &pp->volFogGroundY,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->volFog, pp->locVFTopY, &pp->volFogTopY,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->volFog, pp->locVFCoverage, &pp->volFogCoverage,
                   SHADER_UNIFORM_FLOAT);
    BlitTexture(pp->scene.texture, pp->renderW, pp->renderH, hw, hh);
    EndShaderMode();
    EndTextureMode();

    BeginTextureMode(pp->scene);
    BeginBlendMode(BLEND_ALPHA);
    BlitTexture(pp->volFogTex.texture, hw, hh, pp->renderW, pp->renderH);
    EndBlendMode();
    EndTextureMode();
  }

  // 0. SSAO: reconstruct view-space positions from scene depth, sample the
  //    rotated hemisphere kernel into aoRaw (half-res), then 4x4 box-blur to
  //    aoBlur (kills the rotation dither). Consumed by the composite.
  if (pp->ssaoEnabled) {
    Matrix proj = pp->projectionMatrix;
    Matrix invProj = MatrixInvert(proj);

    BeginTextureMode(pp->aoRaw);
    ClearBackground(WHITE);
    BeginShaderMode(pp->ssao);
    SetShaderValueTexture(pp->ssao, pp->locSsaoDepth, pp->scene.depth);
    SetShaderValueTexture(pp->ssao, pp->locSsaoNoise, pp->noise);
    SetShaderValueMatrix(pp->ssao, pp->locSsaoProj, proj);
    SetShaderValueMatrix(pp->ssao, pp->locSsaoInvProj, invProj);
    float noiseScale[2] = {(float)hw / 4.0f, (float)hh / 4.0f};
    SetShaderValue(pp->ssao, pp->locSsaoNoiseScale, noiseScale,
                   SHADER_UNIFORM_VEC2);
    SetShaderValue(pp->ssao, pp->locSsaoRadius, &pp->ssaoRadius,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->ssao, pp->locSsaoBias, &pp->ssaoBias,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->ssao, pp->locSsaoStrength, &pp->ssaoStrength,
                   SHADER_UNIFORM_FLOAT);
    BlitTexture(pp->scene.texture, pp->renderW, pp->renderH, hw, hh);
    EndShaderMode();
    EndTextureMode();

    BeginTextureMode(pp->aoBlur);
    BeginShaderMode(pp->ssaoBlur);
    float aoTexel[2] = {1.0f / (float)hw, 1.0f / (float)hh};
    SetShaderValue(pp->ssaoBlur, pp->locSsaoBlurTexel, aoTexel,
                   SHADER_UNIFORM_VEC2);
    BlitTexture(pp->aoRaw.texture, hw, hh, hw, hh);
    EndShaderMode();
    EndTextureMode();
  }

  // 1. Bright-pass: scene (full) -> bloomA (half).
  BeginTextureMode(pp->bloomA);
  ClearBackground(BLACK);
  BeginShaderMode(pp->bright);
  SetShaderValue(pp->bright, pp->locBrightThreshold, &pp->bloomThreshold,
                 SHADER_UNIFORM_FLOAT);
  BlitTexture(pp->scene.texture, pp->renderW, pp->renderH, hw, hh);
  EndShaderMode();
  EndTextureMode();

  // 2. Multi-scale Kawase dual-filtering bloom
  // Downsample pass (A -> C -> E)
  KawaseDownsample(pp, pp->bloomA, pp->bloomC, hw, hh, qw, qh);
  KawaseDownsample(pp, pp->bloomC, pp->bloomE, qw, qh, ew, eh);

  // Upsample pass (E -> C -> A)
  KawaseUpsample(pp, pp->bloomE, pp->bloomC, ew, eh, qw, qh, 0.8f);
  KawaseUpsample(pp, pp->bloomC, pp->bloomA, qw, qh, hw, hh, 0.8f);

  // 2b. God rays: raymarch the sun shadow map at quarter res, then a
  //     separable blur smooths the march dither. Skipped (target stays
  //     black) without a shadow pass or with the sun below the horizon.
  bool raysOn = pp->godRaysEnabled && pp->shadowMap.id != 0 &&
                pp->sunDir.y > 0.0f;
  BeginTextureMode(pp->godRayA);
  ClearBackground(BLACK);
  if (raysOn) {
    BeginShaderMode(pp->godrays);
    SetShaderValueTexture(pp->godrays, pp->locGRDepth, pp->scene.depth);
    SetShaderValueTexture(pp->godrays, pp->locGRShadowMap, pp->shadowMap);
    // The shadow map is now a 2x2 atlas; sample the far cascade's tile.
    SetShaderValue(pp->godrays, GetShaderLocation(pp->godrays, "uShadowTile"),
                   &pp->shadowTile, SHADER_UNIFORM_VEC2);
    SetShaderValueMatrix(pp->godrays, pp->locGRInvVP, invViewProj);
    SetShaderValueMatrix(pp->godrays, pp->locGRMatLight, pp->lightVP);
    SetShaderValue(pp->godrays, pp->locGRCamPos, camPos, SHADER_UNIFORM_VEC3);
    SetShaderValue(pp->godrays, pp->locGRSunDir, sunDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(pp->godrays, pp->locGRSunCol, sunCol, SHADER_UNIFORM_VEC3);
    float grRes[2] = {(float)qw, (float)qh};
    SetShaderValue(pp->godrays, pp->locGRRes, grRes, SHADER_UNIFORM_VEC2);
    SetShaderValue(pp->godrays, pp->locGRTime, &time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->godrays, pp->locGRDecay, &pp->godRaysDecay,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->godrays, pp->locGRDensity, &pp->godRaysDensity,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->godrays, pp->locGRWeight, &pp->godRaysWeight,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(pp->godrays, pp->locGRExposure, &pp->godRaysExposure,
                   SHADER_UNIFORM_FLOAT);
    BlitTexture(pp->scene.texture, pp->renderW, pp->renderH, qw, qh);
    EndShaderMode();
  }
  EndTextureMode();
  if (raysOn) BlurLevel(pp, pp->godRayA, pp->godRayB, qw, qh, 2);

  // 3. Composite into the logical-res present target. The composite shader
  //    flips Y (render textures are stored bottom-up); the final blit flips
  //    back, so the parity nets out upright.
  int permIndex = 0;
  if (pp->dofEnabled) permIndex |= 1;
  if (raysOn) permIndex |= 2;
  if (pp->ssaoEnabled) permIndex |= 4;
  
  // Create a local alias for cleaner code
  #define COMP pp->compositePerms[permIndex]

  float res[2] = {(float)pp->outW, (float)pp->outH};
  SetShaderValue(COMP.shader, COMP.locResolution, res, SHADER_UNIFORM_VEC2);
  SetShaderValue(COMP.shader, COMP.locBloomIntensity, &pp->bloomIntensity,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(COMP.shader, COMP.locExposure, &pp->exposure,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(COMP.shader, COMP.locTime, &time, SHADER_UNIFORM_FLOAT);
  SetShaderValue(COMP.shader, COMP.locDisplayP3, &pp->displayP3,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(COMP.shader, COMP.locP3Strength, &pp->p3Strength,
                 SHADER_UNIFORM_FLOAT);

  if (pp->dofEnabled) {
    float nearP = 0.01f, farP = 1000.0f;
    SetShaderValue(COMP.shader, COMP.locDofNear, &nearP, SHADER_UNIFORM_FLOAT);
    SetShaderValue(COMP.shader, COMP.locDofFar, &farP, SHADER_UNIFORM_FLOAT);
    SetShaderValue(COMP.shader, COMP.locDofFocus, &pp->focusDist,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(COMP.shader, COMP.locDofRange, &pp->dofRange,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(COMP.shader, COMP.locDofStrength, &pp->dofStrength,
                   SHADER_UNIFORM_FLOAT);
  }

  BeginTextureMode(pp->present);
  BeginShaderMode(COMP.shader);
  SetShaderValueTexture(COMP.shader, COMP.locBloomTex, pp->bloomA.texture);
  if (pp->ssaoEnabled) {
    SetShaderValueTexture(COMP.shader, COMP.locAOTex, pp->aoBlur.texture);
  }
  if (pp->dofEnabled) {
    SetShaderValueTexture(COMP.shader, COMP.locDofDepth, pp->scene.depth);
  }
  if (raysOn) {
    SetShaderValueTexture(COMP.shader, COMP.locGodRayTex, pp->godRayA.texture);
  }
  BlitTexture(pp->scene.texture, pp->renderW, pp->renderH, pp->outW, pp->outH);
  EndShaderMode();
  EndTextureMode();
  
  #undef COMP

  // 4. SMAA 1x on the LDR present image: edges -> blend weights ->
  //    neighborhood blend. The FBO path has no MSAA, so this is the engine's
  //    edge anti-aliasing. Result feeds the sharpen/upscale.
  if (pp->smaaEnabled) {
    float metrics[4] = {1.0f / (float)pp->outW, 1.0f / (float)pp->outH,
                        (float)pp->outW, (float)pp->outH};
    BeginTextureMode(pp->smaaEdgesTex);
    ClearBackground(BLANK);
    BeginShaderMode(pp->smaaEdges);
    SetShaderValue(pp->smaaEdges, pp->locEdgesMetrics, metrics,
                   SHADER_UNIFORM_VEC4);
    SetShaderValue(pp->smaaEdges, pp->locEdgesThreshold, &pp->smaaThreshold,
                   SHADER_UNIFORM_FLOAT);
    BlitTexture(pp->present.texture, pp->outW, pp->outH, pp->outW, pp->outH);
    EndShaderMode();
    EndTextureMode();

    BeginTextureMode(pp->smaaBlendTex);
    ClearBackground(BLANK);
    BeginShaderMode(pp->smaaWeights);
    SetShaderValue(pp->smaaWeights, pp->locWeightsMetrics, metrics,
                   SHADER_UNIFORM_VEC4);
    SetShaderValueTexture(pp->smaaWeights, pp->locWeightsArea, pp->smaaArea);
    SetShaderValueTexture(pp->smaaWeights, pp->locWeightsSearch,
                          pp->smaaSearch);
    BlitTexture(pp->smaaEdgesTex.texture, pp->outW, pp->outH, pp->outW,
                pp->outH);
    EndShaderMode();
    EndTextureMode();

    BeginTextureMode(pp->presentAA);
    ClearBackground(BLANK);
    BeginShaderMode(pp->smaaBlend);
    SetShaderValue(pp->smaaBlend, pp->locBlendMetrics, metrics,
                   SHADER_UNIFORM_VEC4);
    SetShaderValueTexture(pp->smaaBlend, pp->locBlendWeights,
                          pp->smaaBlendTex.texture);
    BlitTexture(pp->present.texture, pp->outW, pp->outH, pp->outW, pp->outH);
    EndShaderMode();
    EndTextureMode();
  }
}

Texture2D BrushPostGetFinalTexture(const BrushPost *pp) {
  if (pp->smaaEnabled) {
    return pp->presentAA.texture;
  }
  return pp->present.texture;
}

void BrushPostRun(BrushPost *pp, float time) {
  if (!pp->ready) return;
  BrushPostRunNoPresent(pp, time);

  RenderTexture2D *finalSrc = pp->smaaEnabled ? &pp->presentAA : &pp->present;

  // 5. Upscale to the backbuffer with CAS fused into the blit. Only worth it
  //    when the blit actually upscales (render scale < 1 and/or retina 2x).
  bool useSharpen = pp->sharpenEnabled && pp->sharpenAmount > 0.001f &&
                    pp->outW < GetRenderWidth();
  if (useSharpen) {
    float texel[2] = {1.0f / (float)pp->outW, 1.0f / (float)pp->outH};
    SetShaderValue(pp->sharpen, pp->locSharpTexel, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(pp->sharpen, pp->locSharpAmount, &pp->sharpenAmount,
                   SHADER_UNIFORM_FLOAT);
    BeginShaderMode(pp->sharpen);
  }
  DrawTexturePro(
      finalSrc->texture,
      (Rectangle){0, 0, (float)pp->outW, -(float)pp->outH},
      (Rectangle){0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()},
      (Vector2){0, 0}, 0.0f, WHITE);
  if (useSharpen) EndShaderMode();
}
