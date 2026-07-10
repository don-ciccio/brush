/*******************************************************************************************
 *   b_post.c - HDR post-processing (see b_post.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_post.h"

#include <rlgl.h>
#include <stdlib.h>

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
  pp->blurPasses = 2;
  pp->exposure = EnvF("BRUSH_EXPOSURE", 1.0f);
  pp->sharpenEnabled = (getenv("BRUSH_NO_SHARPEN") == NULL);
  pp->sharpenAmount = EnvF("BRUSH_SHARPEN", 0.10f);
#if defined(__APPLE__)
  pp->displayP3 = (getenv("BRUSH_NO_P3") == NULL) ? 1.0f : 0.0f;
#else
  pp->displayP3 = (getenv("BRUSH_DISPLAY_P3") != NULL) ? 1.0f : 0.0f;
#endif
  pp->p3Strength = EnvF("BRUSH_P3_STRENGTH", 1.0f);

  pp->scene = LoadHDRTarget(pp->renderW, pp->renderH, true);
  pp->bloomA = LoadHDRTarget(pp->renderW / 2, pp->renderH / 2, false);
  pp->bloomB = LoadHDRTarget(pp->renderW / 2, pp->renderH / 2, false);
  pp->bloomC = LoadHDRTarget(pp->renderW / 4, pp->renderH / 4, false);
  pp->bloomD = LoadHDRTarget(pp->renderW / 4, pp->renderH / 4, false);
  pp->bloomE = LoadHDRTarget(pp->renderW / 8, pp->renderH / 8, false);
  pp->bloomF = LoadHDRTarget(pp->renderW / 8, pp->renderH / 8, false);
  // LDR composite at logical res; the heavy composite shader runs at 1x and
  // the (retina) backbuffer only pays for the cheap sharpen blit.
  pp->present = LoadRenderTexture(pp->outW, pp->outH);
  SetTextureFilter(pp->present.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(pp->present.texture, TEXTURE_WRAP_CLAMP);

  pp->bright = LoadShader(NULL, "engine/shaders/bloom_bright.fs");
  pp->blur = LoadShader(NULL, "engine/shaders/blur.fs");
  pp->composite = LoadShader(NULL, "engine/shaders/composite.fs");
  pp->sharpen = LoadShader(NULL, "engine/shaders/sharpen.fs");

  pp->locBrightThreshold = GetShaderLocation(pp->bright, "uThreshold");
  pp->locBlurDir = GetShaderLocation(pp->blur, "uDir");
  pp->locBlurTexel = GetShaderLocation(pp->blur, "uTexel");
  pp->locBloomTex = GetShaderLocation(pp->composite, "uBloom");
  pp->locBloomIntensity = GetShaderLocation(pp->composite, "uBloomIntensity");
  pp->locExposure = GetShaderLocation(pp->composite, "uExposure");
  pp->locResolution = GetShaderLocation(pp->composite, "uResolution");
  pp->locTime = GetShaderLocation(pp->composite, "uTime");
  pp->locDisplayP3 = GetShaderLocation(pp->composite, "uDisplayP3");
  pp->locP3Strength = GetShaderLocation(pp->composite, "uP3Strength");
  pp->locSharpTexel = GetShaderLocation(pp->sharpen, "uTexel");
  pp->locSharpAmount = GetShaderLocation(pp->sharpen, "uSharpen");

  pp->ready = (pp->scene.id != 0 && pp->bloomA.id != 0);
  TraceLog(LOG_INFO,
           "BRUSH post: HDR pipeline %s (scene %dx%d, out %dx%d, scale %.2f)",
           pp->ready ? "ready" : "UNAVAILABLE", pp->renderW, pp->renderH,
           pp->outW, pp->outH, renderScale);
}

void BrushPostUnload(BrushPost *pp) {
  UnloadHDRTarget(&pp->scene);
  UnloadHDRTarget(&pp->bloomA);
  UnloadHDRTarget(&pp->bloomB);
  UnloadHDRTarget(&pp->bloomC);
  UnloadHDRTarget(&pp->bloomD);
  UnloadHDRTarget(&pp->bloomE);
  UnloadHDRTarget(&pp->bloomF);
  UnloadRenderTexture(pp->present);
  UnloadShader(pp->bright);
  UnloadShader(pp->blur);
  UnloadShader(pp->composite);
  UnloadShader(pp->sharpen);
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

// Additively accumulate `src` up into the current target, scaled by `weight`
// so the wider bloom mips stay below the tight core.
static void AddUpsample(Texture2D src, int srcW, int srcH, int destW,
                        int destH, float weight) {
  unsigned char w = (unsigned char)(weight * 255.0f);
  BeginBlendMode(BLEND_ADDITIVE);
  DrawTexturePro(src, (Rectangle){0, 0, (float)srcW, (float)srcH},
                 (Rectangle){0, 0, (float)destW, (float)destH}, (Vector2){0, 0},
                 0.0f, (Color){255, 255, 255, w});
  EndBlendMode();
}

void BrushPostRun(BrushPost *pp, float time) {
  if (!pp->ready) return;
  int hw = pp->renderW / 2, hh = pp->renderH / 2;
  int qw = pp->renderW / 4, qh = pp->renderH / 4;
  int ew = pp->renderW / 8, eh = pp->renderH / 8;

  // 1. Bright-pass: scene (full) -> bloomA (half).
  BeginTextureMode(pp->bloomA);
  ClearBackground(BLACK);
  BeginShaderMode(pp->bright);
  SetShaderValue(pp->bright, pp->locBrightThreshold, &pp->bloomThreshold,
                 SHADER_UNIFORM_FLOAT);
  BlitTexture(pp->scene.texture, pp->renderW, pp->renderH, hw, hh);
  EndShaderMode();
  EndTextureMode();

  // 2. Multi-scale bloom: blur each mip, then upsample-accumulate the wider
  //    levels back into the half-res core (weights < 1 keep the core dominant).
  BlurLevel(pp, pp->bloomA, pp->bloomB, hw, hh, pp->blurPasses);

  BeginTextureMode(pp->bloomC);
  ClearBackground(BLACK);
  BlitTexture(pp->bloomA.texture, hw, hh, qw, qh);
  EndTextureMode();
  BlurLevel(pp, pp->bloomC, pp->bloomD, qw, qh, pp->blurPasses);

  BeginTextureMode(pp->bloomE);
  ClearBackground(BLACK);
  BlitTexture(pp->bloomC.texture, qw, qh, ew, eh);
  EndTextureMode();
  BlurLevel(pp, pp->bloomE, pp->bloomF, ew, eh, pp->blurPasses);

  BeginTextureMode(pp->bloomC);
  AddUpsample(pp->bloomE.texture, ew, eh, qw, qh, 0.8f);
  EndTextureMode();
  BeginTextureMode(pp->bloomA);
  AddUpsample(pp->bloomC.texture, qw, qh, hw, hh, 0.8f);
  EndTextureMode();

  // 3. Composite into the logical-res present target. The composite shader
  //    flips Y (render textures are stored bottom-up); the final blit flips
  //    back, so the parity nets out upright.
  float res[2] = {(float)pp->outW, (float)pp->outH};
  SetShaderValue(pp->composite, pp->locResolution, res, SHADER_UNIFORM_VEC2);
  SetShaderValue(pp->composite, pp->locBloomIntensity, &pp->bloomIntensity,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(pp->composite, pp->locExposure, &pp->exposure,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(pp->composite, pp->locTime, &time, SHADER_UNIFORM_FLOAT);
  SetShaderValue(pp->composite, pp->locDisplayP3, &pp->displayP3,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(pp->composite, pp->locP3Strength, &pp->p3Strength,
                 SHADER_UNIFORM_FLOAT);

  BeginTextureMode(pp->present);
  BeginShaderMode(pp->composite);
  SetShaderValueTexture(pp->composite, pp->locBloomTex, pp->bloomA.texture);
  BlitTexture(pp->scene.texture, pp->renderW, pp->renderH, pp->outW, pp->outH);
  EndShaderMode();
  EndTextureMode();

  // 4. Upscale to the backbuffer with CAS fused into the blit. Only worth it
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
      pp->present.texture,
      (Rectangle){0, 0, (float)pp->outW, -(float)pp->outH},
      (Rectangle){0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()},
      (Vector2){0, 0}, 0.0f, WHITE);
  if (useSharpen) EndShaderMode();
}
