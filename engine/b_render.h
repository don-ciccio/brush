/*******************************************************************************************
 *   b_render.h - Layered render pipeline
 *
 *   The frame is built as an ordered stack of layers; a game submits draws
 *   into a layer and the engine executes the stack:
 *
 *     SHADOW       sun depth pass (v0: accepted but not executed yet — the
 *                  shadow-map port slots in here without API changes)
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

// Access the post pipeline's tunables (bloom threshold/intensity, exposure,
// sharpen). Returns NULL if the HDR target could not be created.
struct BrushPost *BrushRenderGetPost(void);

// The engine's forward lit shader. Assign it to your models' materials:
//   model.materials[i].shader = BrushGetLitShader();
Shader BrushGetLitShader(void);

// Single directional sun shared by the lit shader and the sky. `dir` points
// TOWARD the sun. `ambient` is a flat ambient term (0..1).
void BrushSetSun(Vector3 dir, Vector3 color, float ambient);
Vector3 BrushGetSunDir(void);

void BrushSetSkyEnabled(bool enabled);

// Submit one model draw into a layer for this frame. The transform is a full
// world matrix; the model's own transform is ignored for this draw.
void BrushRenderSubmit(BrushLayer layer, Model *model, Matrix transform,
                       Color tint);

// Execute the layer stack for this frame (inside BeginDrawing). Clears all
// submission lists afterwards.
void BrushRenderExecute(Camera3D camera);

// Debug layer view (F2 handled by b_app).
void BrushRenderCycleLayerView(void);
BrushLayerView BrushRenderGetLayerView(void);
const char *BrushRenderLayerViewName(void);

#endif // B_RENDER_H
