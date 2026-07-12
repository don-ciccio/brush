/*******************************************************************************************
 *   b_render.h - Layered render pipeline
 *
 *   The frame is built as an ordered stack of layers; a game submits draws
 *   into a layer and the engine executes the stack:
 *
 *     SHADOW       sun depth pass: casters submitted here render into the
 *                  shadow map (b_shadow.h); the lit shader tests receivers
 *                  against it with PCSS soft shadows
 *     OPAQUE       base color + direct lighting (the "color pass": albedo,
 *                  diffuse sunlight, specular highlights in one forward
 *                  shader for now — see uLayerView for the decomposition)
 *     SKY          procedural sky dome, drawn AFTER opaque geometry at the
 *                  far plane so early-Z rejects covered pixels
 *     TRANSPARENT  alpha-blended, sorted back-to-front, depth-write off
 *     VOLUME       volumetrics (fog, god rays) — reserved, executes empty
 *
 *   With the HDR post pipeline enabled (default), the whole stack renders
 *   into a linear RGBA16F target and b_post does bloom -> ACES tone map ->
 *   grade -> CAS-sharpened upscale to the backbuffer (see b_post.h).
 *
 *   F2 (BrushRenderCycleLayerView) isolates the lighting terms of the opaque
 *   pass on screen: final / albedo / diffuse / specular / normals. The same
 *   decomposition later becomes real intermediate targets when the HDR
 *   post-processing pipeline is ported.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_RENDER_H
#define B_RENDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <raylib.h>
#include <stdbool.h>

typedef enum {
  BRUSH_LAYER_SHADOW = 0,
  BRUSH_LAYER_OPAQUE,
  BRUSH_LAYER_SKY,
  BRUSH_LAYER_TRANSPARENT,
  BRUSH_LAYER_VOLUME,
  BRUSH_LAYER_COUNT
} BrushLayer;

// Lighting-term views for the opaque layer (cycled with F2).
typedef enum {
  BRUSH_VIEW_FINAL = 0,
  BRUSH_VIEW_ALBEDO,
  BRUSH_VIEW_DIFFUSE,
  BRUSH_VIEW_SPECULAR,
  BRUSH_VIEW_NORMALS,
  BRUSH_VIEW_SHADOW, // sun visibility term (white = lit, black = shadowed)
  BRUSH_VIEW_COUNT
} BrushLayerView;

// Called by BrushRun. `width/height` is the logical resolution; `renderScale`
// (0.25..1, 0 -> 1.0) sizes the internal HDR scene target (see b_post.h).
void BrushRenderInit(int width, int height, float renderScale);
void BrushRenderShutdown(void);

// HDR post pipeline toggle (F3; starts enabled unless BRUSH_NO_POST is set).
// While a debug layer view is active the post pass is bypassed automatically
// so the isolated lighting terms stay readable (no tone map / grade on them).
void BrushRenderTogglePost(void);
bool BrushRenderIsPostEnabled(void);

// Sun shadow toggle (F4; starts enabled unless BRUSH_NO_SHADOW is set).
void BrushRenderToggleShadows(void);
bool BrushRenderShadowsEnabled(void);

// Access the post pipeline's tunables (bloom threshold/intensity, exposure,
// sharpen). Returns NULL if the HDR target could not be created.
struct BrushPost *BrushRenderGetPost(void);

// Access the sun-shadow state (softness is the editor-facing tunable).
struct BrushShadow *BrushRenderGetShadow(void);

// Set whether the renderer is in editor mode (disables final screen presentation in post-processing).
void BrushRenderSetEditorMode(bool enabled);

// Recreate the post pipeline's targets for a new output size (call when the
// window resizes; keeps the render scale chosen at init).
void BrushRenderResize(int width, int height);

// The engine's forward lit shader. Assign it to your models' materials:
//   model.materials[i].shader = BrushGetLitShader();
Shader BrushGetLitShader(void);

// Single directional light shared by the lit shader, shadow pass, and (by
// default) the sky. `dir` points TOWARD the light. `ambient` is the ambient
// fill color (linear).
void BrushSetSun(Vector3 dir, Vector3 color, Vector3 ambient);
Vector3 BrushGetSunDir(void);

// --- Point lights (DYNAMIC, never baked — see docs/roadmap.md) -------------
// Submit per frame, like draws: the list clears after every execute, so
// moving lights (a carried torch) are free. Colors are LINEAR and may exceed
// 1.0 for intensity (the HDR pipeline blooms them naturally). `radius` is
// where the light's influence smoothly reaches zero. If more than
// BRUSH_MAX_POINT_LIGHTS are submitted, the ones nearest the camera win.
#define BRUSH_MAX_POINT_LIGHTS 8

typedef struct BrushPointLight {
  Vector3 position;
  Vector3 color;  // linear; scale for intensity
  float radius;   // metres to zero influence
} BrushPointLight;

void BrushRenderSubmitPointLight(BrushPointLight light);

// Drive the whole frame's lighting from a day/night clock (see b_tod.h):
// sun direction/color, ambient fill, sky sun+moon, post exposure. At night
// (sun below the horizon) the MOON becomes the directional light — the
// engine's one light authority is animated, never duplicated. Call once per
// frame after BrushTodUpdate; games with a static sun just never call it.
struct BrushTimeOfDay;
void BrushRenderApplyTimeOfDay(const struct BrushTimeOfDay *tod);

void BrushSetSkyEnabled(bool enabled);

// Submit one model draw into a layer for this frame. The transform is a full
// world matrix; the model's own transform is ignored for this draw.
void BrushRenderSubmit(BrushLayer layer, Model *model, Matrix transform,
                       Color tint);

// --- Per-draw material overrides -------------------------------------------
// Textured surfaces without authored UVs (scene blocks, blockout geometry):
// with `triplanar` set, the lit shader samples by world position on the
// three axes — scaled/rotated boxes tile seamlessly at `texScale` world
// metres per repeat, no tangents needed. Models with real UVs skip triplanar
// and get tangent-space normal mapping when `normal` is set.
typedef struct BrushMaterialProps {
  Texture2D albedo;   // id 0 = keep the model's own diffuse map
  Texture2D normal;   // id 0 = no normal mapping
  Texture2D displacement; // id 0 = no displacement mapping
  Texture2D ao;           // id 0 = no ambient occlusion map
  bool triplanar;     // world-space projection instead of mesh UVs
  bool normalSwizzled; // normal map is DXT5nm (X in alpha, Y in green);
                       // ask BrushAssetsIsSwizzledNormal for cached textures
  float texScale;     // world metres per texture repeat (triplanar)
  float specStrength; // <0 = engine default
  float normalDepth;  // normal map intensity (1 = authored)
  float heightScale;  // displacement scale strength (default 0.05)
  float aoStrength;   // ambient occlusion strength (default 1.0)
  bool parallax;      // ray-march the height map (POM) — needs displacement
} BrushMaterialProps;

// BrushRenderSubmit with material overrides applied for exactly this draw.
// NULL props = plain submit.
void BrushRenderSubmitEx(BrushLayer layer, Model *model, Matrix transform,
                         Color tint, const BrushMaterialProps *props);

// --- Terrain splat (painted layer blending — docs/terrain-painting-plan.md) --
// Up to 4 paintable layers, each a material-library texture set. A chunk
// submits its mesh with a per-chunk RGBA8 weight texture; the lit shader
// blends the layers planar-XZ at each layer's own tile scale.
#define BRUSH_TERRAIN_LAYERS 4

typedef struct BrushTerrainLayer {
  Texture2D albedo;
  Texture2D normal;    // id 0 = flat
  float tile;          // world metres per texture repeat
  bool normalSwizzled; // DXT5nm-cooked normal map
} BrushTerrainLayer;

typedef struct BrushSplatDraw {
  Texture2D splat;    // per-chunk weight texture (RGBA8 = layers 0..3)
  Vector3 origin;     // chunk world origin (XZ used)
  float size;         // chunk world size (metres)
  int res;            // splat texture resolution per side
  BrushTerrainLayer layers[BRUSH_TERRAIN_LAYERS];
  int layerCount;
  // Auto-slope mask: steep terrain blends toward `autoSlopeLayer` beneath
  // the painted weights (-1 = off). Angles in degrees from horizontal.
  int autoSlopeLayer;
  float autoSlopeStart, autoSlopeEnd;
  // Per-layer auto-height bands (index by layer slot). on[i]=1 -> layer i
  // fades in between start[i] and full[i] (full>start = up/snowline,
  // full<start = down/shoreline). Applied beneath the paint, in slot order.
  int   layerHeightOn[BRUSH_TERRAIN_LAYERS];
  float layerHeightStart[BRUSH_TERRAIN_LAYERS];
  float layerHeightFull[BRUSH_TERRAIN_LAYERS];
} BrushSplatDraw;

// BrushRenderSubmitMesh with splat blending for exactly this draw.
void BrushRenderSubmitMeshSplat(BrushLayer layer, Mesh mesh,
                                Material *material, Matrix transform,
                                const BrushSplatDraw *splat);

// Submit a raw mesh + material (e.g. streamed terrain chunks). The material's
// shader should be BrushGetLitShader() to receive lighting/shadows. `material`
// must outlive the frame (a pointer, not copied).
void BrushRenderSubmitMesh(BrushLayer layer, Mesh mesh, Material *material,
                           Matrix transform);

// Execute the layer stack for this frame (inside BeginDrawing). Clears all
// submission lists afterwards.
void BrushRenderExecute(Camera3D camera);

// Debug layer view (F2 handled by b_app).
void BrushRenderCycleLayerView(void);
BrushLayerView BrushRenderGetLayerView(void);
const char *BrushRenderLayerViewName(void);

// --- Frustum culling ---------------------------------------------------------
// Six view-frustum planes (x,y,z = unit normal pointing INTO the frustum,
// w = signed offset), extracted from the SAME projection the renderer draws
// with (BeginMode3D: camera fovy, screen aspect, rl cull near/far). Build one
// per frame from the render camera, then test object bounds before submitting.
typedef struct BrushFrustum {
  Vector4 planes[6];
} BrushFrustum;

BrushFrustum BrushRenderMakeFrustum(Camera3D camera);

// True if the sphere is inside or touching the frustum (conservative).
bool BrushFrustumContainsSphere(const BrushFrustum *f, Vector3 center,
                                float radius);

// Conservative world bounding sphere of a local AABB under a transform: all 8
// corners are transformed (handles rotation + non-uniform scale).
void BrushBoundingSphere(BoundingBox local, Matrix transform, Vector3 *center,
                         float *radius);

#ifdef __cplusplus
}
#endif

#endif // B_RENDER_H
