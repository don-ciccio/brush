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

#ifdef __cplusplus
extern "C" {
#endif

#include <raylib.h>
#include <stdbool.h>

#define BRUSH_SSAO_KERNEL 24

typedef struct BrushPost {
  int outW, outH;       // logical output resolution (composite/present res)
  int renderW, renderH; // internal HDR scene resolution (outW * renderScale)

  RenderTexture2D scene;  // HDR (RGBA16F) + depth texture — the scene target
  RenderTexture2D bloomA; // 1/2-res HDR ping-pong (mip 0)
  RenderTexture2D bloomC; // 1/4-res HDR (mip 1)
  RenderTexture2D bloomE; // 1/8-res HDR (mip 2, widest)
  RenderTexture2D present; // LDR composite at logical res, upscaled at the end
  RenderTexture2D aoRaw;   // half-render-res SSAO (R), 8-bit
  RenderTexture2D aoBlur;  // 4x4 box-blurred SSAO
  RenderTexture2D smaaEdgesTex, smaaBlendTex, presentAA; // SMAA pass targets
  RenderTexture2D godRayA, godRayB; // quarter-render-res HDR shaft targets
  RenderTexture2D volFogTex;        // half-render-res HDR fog (straight alpha)

  Shader bright;
  Shader kawaseDown;
  Shader kawaseUp;
  Shader blur; // still used? maybe remove? wait, maybe keep it in case.
  Shader sharpen;
  Shader ssao, ssaoBlur;
  Shader smaaEdges, smaaWeights, smaaBlend;
  Shader godrays; // quarter-res shadow-map raymarch (godrays.fs)
  Shader volFog;  // half-res height-fog raymarch (volfog.fs)
  Texture2D noise;               // 4x4 SSAO rotation vectors
  Texture2D smaaArea, smaaSearch; // SMAA precomputed lookups (assets/smaa)

  struct {
    Shader shader;
    int locBloomTex, locBloomIntensity, locExposure, locResolution, locTime;
    int locDisplayP3, locP3Strength;
    int locAOTex, locAOEnabled;
    int locDofDepth, locDofNear, locDofFar, locDofFocus, locDofRange;
    int locDofStrength, locDofEnabled;
    int locGodRayTex, locGodRaysOn;
  } compositePerms[8]; // 3 bits: [0]=DOF, [1]=GodRays, [2]=AO

  int locBrightThreshold;

  int locKawaseDownTexel, locKawaseUpTexel;
  int locBlurDir, locBlurTexel;
  int locSharpTexel, locSharpAmount;
  int locSsaoDepth, locSsaoNoise, locSsaoKernel, locSsaoProj, locSsaoInvProj;
  int locSsaoNoiseScale, locSsaoRadius, locSsaoBias, locSsaoStrength;
  int locSsaoBlurTexel;
  int locEdgesMetrics, locEdgesThreshold;
  int locWeightsMetrics, locWeightsArea, locWeightsSearch;
  int locBlendMetrics, locBlendWeights;
  int locGRDepth, locGRShadowMap, locGRInvVP, locGRMatLight, locGRCamPos;
  int locGRSunDir, locGRSunCol, locGRRes, locGRTime;
  int locGRDecay, locGRDensity, locGRWeight, locGRExposure;
  int locVFDepth, locVFInvVP, locVFCamPos, locVFSunDir, locVFSunCol;
  int locVFSkyCol, locVFTime, locVFWind;
  int locVFDensity, locVFGroundY, locVFTopY, locVFCoverage;

  // tunables (env-overridable, see b_post.c)
  float bloomThreshold; // luminance above which pixels bloom
  float bloomIntensity; // how strongly bloom is added back
  int blurPasses;       // gaussian ping-pong iterations per mip
  float exposure;       // pre-tonemap multiplier
  bool sharpenEnabled;
  float sharpenAmount; // 0..1 CAS strength
  float displayP3;     // 1 = apply the sRGB->P3 gamut map (Apple wide-gamut)
  float p3Strength;    // 1 = accurate map, lower = punchier

  // SSAO (depth-based, half render-res): grounds objects with contact
  // shading. Needs the scene camera matrices, captured by the renderer
  // inside BeginMode3D each frame.
  bool ssaoEnabled;
  float ssaoRadius;   // world-space sampling radius
  float ssaoBias;     // self-occlusion bias
  float ssaoStrength; // how strongly AO darkens
  float ssaoKernel[BRUSH_SSAO_KERNEL * 3];
  Matrix projectionMatrix, viewMatrix;

  // SMAA 1x (3 passes on the LDR present image) — the FBO path has no MSAA,
  // so this is the engine's edge anti-aliasing.
  bool smaaEnabled;
  float smaaThreshold; // edge threshold (0.05..0.1)

  // Depth of field (far-only bokeh in the composite): keeps subject and
  // foreground crisp, softens only the distance — an atmospheric hint, not
  // a lens sim.
  bool dofEnabled;
  float dofRange;    // metres from the focus plane to full blur
  float dofStrength; // max sharp->blur blend (0..1)

  // God rays: quarter-res raymarch of the sun shadow map, blurred, added
  // over the scene in the composite. Needs the shadow pass to have run.
  bool godRaysEnabled;
  float godRaysDecay;    // max ray length (1.0 = 100 m)
  float godRaysDensity;  // scattering density multiplier
  float godRaysWeight;   // Henyey-Greenstein anisotropy g
  float godRaysExposure; // overall shaft brightness

  // Volumetric ground fog (half-res raymarch, alpha over the scene BEFORE
  // bloom/god rays). OFF by default — enable with BRUSH_VOLFOG=1 or the F9
  // toggle; it's a scene-mood feature, not a pipeline default.
  bool volFogEnabled;
  float volFogDensity;  // base extinction per metre
  float volFogGroundY;  // altitude where fog is densest (m)
  float volFogTopY;     // metres above groundY where fog fades out
  float volFogCoverage; // 0..1 — higher = fewer/smaller banks

  // Per-frame scene state for the depth/volumetric passes, set by the
  // renderer each frame before BrushPostRun (same pattern as the SSAO
  // camera matrices above).
  Vector3 cameraPos;
  float focusDist;     // auto-DOF focus distance (camera -> target)
  Vector3 sunDir;      // current light direction (sun or moon)
  Vector3 sunColor;
  Vector3 ambientColor; // sky/fill colour (fog body tint)
  Matrix lightVP;       // shadow-pass light matrix
  Texture2D shadowMap;  // shadow depth map; id 0 = no shadow pass this frame

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

// Bloom + composite + AA (same as BrushPostRun but skips the final blit to screen/backbuffer).
void BrushPostRunNoPresent(BrushPost *pp, float time);

// Get the final composited LDR texture (either presentAA or present, depending on SMAA).
Texture2D BrushPostGetFinalTexture(const BrushPost *pp);

#ifdef __cplusplus
}
#endif

#endif // B_POST_H
