/*******************************************************************************************
 *   b_post.h - HDR post-processing (bloom + tone map + grade + sharpen)
 *
 *   The scene renders into a floating-point HDR target (linear, un-tonemapped)
 *   at an internal render scale. BrushPostRun then:
 *
 *     1. bright-pass extracts HDR highlights (hue-preserving soft threshold)
 *     2. multi-scale bloom: blur a mip chain (1/2 -> 1/4 -> 1/8) and additively
 *        upsample the wider levels back — a tight bright core plus a soft wide
 *        halo, far more natural than a single-radius glow
 *     3. composite at logical res: bloom add -> exposure -> ACES tone map ->
 *        CDL grade -> contrast pivot -> vibrance/saturation -> vignette ->
 *        film grain -> optional Display P3 gamut map -> sRGB gamma
 *     4. final upscale to the backbuffer with CAS (contrast-adaptive sharpen)
 *        fused into the blit, recovering crispness lost to the render scale
 *
 *   This is the ONLY place tone mapping happens — surface and sky shaders
 *   output linear HDR so highlights have real headroom to bloom.
 *
 *   The render scale is the main GPU-cost lever: every heavy fragment pass
 *   runs at scale^2 of the window pixels; the HUD stays at native res.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_POST_H
#define B_POST_H

#include <raylib.h>
#include <stdbool.h>

typedef struct BrushPost {
  int outW, outH;       // logical output resolution (composite/present res)
  int renderW, renderH; // internal HDR scene resolution (outW * renderScale)

  RenderTexture2D scene;  // HDR (RGBA16F) + depth texture — the scene target
  RenderTexture2D bloomA; // 1/2-res HDR ping-pong (mip 0)
  RenderTexture2D bloomB;
  RenderTexture2D bloomC; // 1/4-res HDR (mip 1)
  RenderTexture2D bloomD;
  RenderTexture2D bloomE; // 1/8-res HDR (mip 2, widest)
  RenderTexture2D bloomF;
  RenderTexture2D present; // LDR composite at logical res, upscaled at the end

  Shader bright;
  Shader blur;
  Shader composite;
  Shader sharpen;

  int locBrightThreshold;
  int locBlurDir, locBlurTexel;
  int locBloomTex, locBloomIntensity, locExposure, locResolution, locTime;
  int locDisplayP3, locP3Strength;
  int locSharpTexel, locSharpAmount;

  // tunables (env-overridable, see b_post.c)
  float bloomThreshold; // luminance above which pixels bloom
  float bloomIntensity; // how strongly bloom is added back
  int blurPasses;       // gaussian ping-pong iterations per mip
  float exposure;       // pre-tonemap multiplier
  bool sharpenEnabled;
  float sharpenAmount; // 0..1 CAS strength
  float displayP3;     // 1 = apply the sRGB->P3 gamut map (Apple wide-gamut)
  float p3Strength;    // 1 = accurate map, lower = punchier

  bool ready; // false if the HDR framebuffer failed (post silently disabled)
} BrushPost;

// `width/height` is the logical output resolution; `renderScale` (0.25..1)
// sizes the internal HDR scene target.
void BrushPostInit(BrushPost *pp, int width, int height, float renderScale);
void BrushPostUnload(BrushPost *pp);

// Render the 3D scene between these two calls (they wrap BeginTextureMode on
// the HDR target and clear it).
void BrushPostBeginScene(BrushPost *pp);
void BrushPostEndScene(BrushPost *pp);

// Bloom + composite + sharpen-upscale to the active target (the backbuffer).
// `time` drives the film-grain dither.
void BrushPostRun(BrushPost *pp, float time);

#endif // B_POST_H
