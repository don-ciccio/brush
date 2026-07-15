/*******************************************************************************************
 *   b_scene.h - World definition files (world.def)
 *
 *   A scene is DATA: authored placements loaded from a plain-text file, so a
 *   new level is a file edit, not a recompile. v1 carries the zero-asset
 *   primitives — colored collider boxes, point lights, the spawn point, and
 *   the time-of-day — which is everything the sandbox gym needs and the
 *   substrate the in-game editor will write (see docs/roadmap.md, authoring
 *   track). Model props join when the asset manager lands (names -> paths).
 *
 *   Format: one entity per line, '#' comments, unknown lines are skipped
 *   with a warning (old engines open newer files):
 *
 *     version 2
 *     spawn 0 0.5 8
 *     time 10.5
 *     material <name> <albedo|-> <normal|-> <tile> <spec> <depth>
 *     block  <x y z> <rx ry rz> <sx sy sz> <r g b> [material|-]
 *     light  <x y z> <r g b (linear, may exceed 1)> <radius> <flicker 0|1>
 *     post   <key> <value>
 *
 *   Materials are triplanar world-space texture sets (albedo + optional
 *   normal map) blocks reference by name; `tile` is world metres per texture
 *   repeat. `post` lines persist the render tunables the editor exposes
 *   (exposure, DOF, god rays, fog...) so a scene carries its look.
 *
 *   Hot reload: BrushSceneHotReload re-loads when the file's mtime changes —
 *   edit the file in any editor while the game runs and watch it move.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_SCENE_H
#define B_SCENE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "b_render.h" // BrushPointLight

#include <raylib.h>
#include <stdbool.h>

#define BRUSH_SCENE_MAX_BLOCKS 256
#define BRUSH_SCENE_MAX_LIGHTS 64
#define BRUSH_SCENE_MAX_MATERIALS 32
#define BRUSH_SCENE_MAX_MODELS 128
#define BRUSH_SCENE_MAX_POST 48
#define BRUSH_SCENE_MAX_ROADS 32
#define BRUSH_SCENE_MAX_FOLIAGE 8
#define BRUSH_SCENE_PATH_MAX 512
#define BRUSH_SCENE_NAME_MAX 32

// A named triplanar texture set. Paths are project-relative; the resolved
// Texture2D handles come from the asset registry (b_assets) via
// BrushSceneResolveMaterials and are id 0 until then / when a path is empty.
typedef struct BrushSceneMaterial {
  char name[BRUSH_SCENE_NAME_MAX];
  char albedo[128];
  char normal[128];
  char displacement[128];
  char ao[128];
  float tile;        // world metres per texture repeat
  float spec;        // specular strength (engine default 0.35)
  float normalDepth; // normal map intensity (1 = authored)
  float heightScale; // displacement scale strength (default 0.05)
  float aoStrength;  // ambient occlusion strength (default 1.0)
  bool uvProjection; // sample by the mesh's authored UVs instead of
                     // triplanar (textures baked FOR a model, e.g. rocks)
  bool parallax;     // ray-march the displacement map (POM) — needs a height map
  bool heightBlend;  // height-based edge blend vs neighbouring terrain layers
  float blendSharp;  // height-blend transition band (smaller = crisper, ~0.2)
  Texture2D albedoTex, normalTex, displacementTex, aoTex; // resolved, not saved
  bool normalGenerated; // normalTex was derived from albedo (owned, not cached)
} BrushSceneMaterial;

// A placed 3D model (static prop). The resolved Model comes from the
// shared registry (b_assets) — instances share meshes/materials, so this
// is for STATIC geometry; animated things own their models. Path is
// project-relative, no spaces (the format is space-separated).
typedef struct BrushSceneModelInstance {
  char path[128]; // e.g. "assets/models/rock_0.glb"
  Vector3 pos;
  Vector3 rot;   // Euler degrees, same convention as blocks
  Vector3 scale; // per-axis; (1,1,1) default
  char material[BRUSH_SCENE_NAME_MAX]; // "" = the model's own textures
  Model model;   // resolved at runtime (meshCount 0 until then), not saved
} BrushSceneModelInstance;

typedef struct BrushSceneBlock {
  Vector3 pos, size;
  Vector3 rot; // Euler angles in degrees (pitch, yaw, roll)
  Color color;
  char material[BRUSH_SCENE_NAME_MAX]; // "" = plain vertex color
} BrushSceneBlock;

// One persisted render tunable ("post exposure 1.2").
typedef struct BrushScenePostSetting {
  char key[24];
  float value;
} BrushScenePostSetting;

typedef struct BrushSceneLight {
  BrushPointLight light;
  bool flicker; // game decides what flicker looks like
} BrushSceneLight;

typedef struct BrushSceneRoad {
  char  material[64];   // terrain-layer material name (resolved to a slot 0..3)
  float width;          // full-weight corridor width (m)
  float fade;           // HEIGHT shoulder: ground eases back over this margin (m)
  float paintFade;      // TEXTURE edge: 0 = hard (paving), >0 = feathered (dirt)
  Vector3 points[32];   // control points (Y matters: it's the road surface)
  int   pointCount;
} BrushSceneRoad;

// One instanced-foliage layer (a plant type scattered over the terrain). Pure
// data; the model/albedo paths resolve via b_assets like a material, falling
// back to the engine's procedural tuft/gradient when empty. The game converts
// this to a BrushFoliageLayerConfig (b_foliage) — b_scene stays foliage-agnostic.
#define BRUSH_SCENE_FOLIAGE_MODELS 4 // == BRUSH_FOLIAGE_MODELS_PER_LAYER

typedef struct BrushSceneFoliageLayer {
  char name[BRUSH_SCENE_NAME_MAX];
  char models[BRUSH_SCENE_FOLIAGE_MODELS][128]; // .glb palette (mixed per
                                                // instance); empty -> procedural
  float modelScale[BRUSH_SCENE_FOLIAGE_MODELS]; // per-variant scale x layer scale (0->1)
  int  modelCount;
  char albedo[128];  // shared fallback card; "" -> model's own / procedural
  float density;       // instances / m^2
  float drawDistance;  // hard cull (m)
  float lodDistance;   // near -> far LOD switch (m)
  float scale, scaleJitter, heightOffset, maxSlopeDeg, windStrength, farKeepRatio;
  Vector3 tint, macroLow, macroHigh;
  int   growLayer;     // grow only where terrain layer N dominates (-1 = any)
  int   avoidLayer;    // exclude where terrain layer N > threshold (-1 = none)
  float avoidThreshold; // 0..1 (0 -> default 0.5)
  bool  avoidRoad;     // exclude foliage from the road surface (default on)
  Model modelRes[BRUSH_SCENE_FOLIAGE_MODELS]; // resolved (not saved)
  Texture2D albedoTex; // resolved (not saved)
} BrushSceneFoliageLayer;

typedef struct BrushScene {
  int version; // scene-file format version read on load (0 = none/fresh). v3+
               // is foliage-aware, so a v3 scene with 0 layers means the author
               // deliberately has no foliage (don't bootstrap a default).
  Vector3 spawn;
  float timeHours; // starting clock (b_tod), <0 = not specified

  BrushSceneBlock blocks[BRUSH_SCENE_MAX_BLOCKS];
  int blockCount;
  BrushSceneLight lights[BRUSH_SCENE_MAX_LIGHTS];
  int lightCount;
  BrushSceneRoad roads[BRUSH_SCENE_MAX_ROADS];
  int roadCount;
  BrushSceneFoliageLayer foliage[BRUSH_SCENE_MAX_FOLIAGE];
  int foliageCount;
  BrushSceneMaterial materials[BRUSH_SCENE_MAX_MATERIALS];
  int materialCount;
  BrushSceneModelInstance models[BRUSH_SCENE_MAX_MODELS];
  int modelCount;
  BrushScenePostSetting post[BRUSH_SCENE_MAX_POST];
  int postCount;
  // Paintable terrain layer slots -> material-library names ("" = unset).
  char terrainLayers[BRUSH_TERRAIN_LAYERS][BRUSH_SCENE_NAME_MAX];
  // Auto-slope mask config (terrain_auto_slope line; layer -1 = off).
  int autoSlopeLayer;
  float autoSlopeStart, autoSlopeEnd; // degrees
  // Per-layer auto-height bands (terrain_layer_height lines): layer i fades
  // in between start[i] and full[i] (full>start = up, full<start = down);
  // on[i]=0 disables. Lets every configured layer be placed by altitude.
  int   layerHeightOn[BRUSH_TERRAIN_LAYERS];
  float layerHeightStart[BRUSH_TERRAIN_LAYERS];
  float layerHeightFull[BRUSH_TERRAIN_LAYERS];

  // Hot-reload bookkeeping (managed by Load/HotReload).
  char path[BRUSH_SCENE_PATH_MAX];
  long modTime;
} BrushScene;

// Load `path` into `s` (replacing its contents). False if the file is
// missing/unreadable — `s` is left zeroed so a bootstrap can fill and Save it.
bool BrushSceneLoad(BrushScene *s, const char *path);

// Write the scene as a world.def text file (and adopt the file's new mtime
// so saving never triggers a self hot-reload). Returns false on I/O failure.
bool BrushSceneSave(BrushScene *scene, const char *path);
// Euler XYZ (degrees) rotation matrix — the one convention shared by the
// editor gizmo, block rendering, and box colliders.
Matrix BrushEulerXYZ(Vector3 degrees);
Matrix BrushBlockGetModelMatrix(const BrushSceneBlock *k);
// Full instance matrix for a placed model: the model's own base transform
// (glTF axis conversion etc.) THEN scale/rot/translate. Render and physics
// both use this — one chokepoint, like BrushBlockGetModelMatrix.
Matrix BrushModelInstanceMatrix(const BrushSceneModelInstance *m);

// Re-load if the file changed on disk since the last Load/HotReload. Returns
// true when a reload happened (the game should re-apply colliders, and
// render settings via BrushSceneApplyRenderSettings).
bool BrushSceneHotReload(BrushScene *s);

// --- Materials --------------------------------------------------------------
// Index of a material by name, -1 if absent/empty.
int BrushSceneFindMaterial(const BrushScene *s, const char *name);

// (Re-)acquire every material's textures AND every model instance from the
// asset registry, releasing whatever was previously resolved. Load calls
// this; the editor calls it again after edits. Idempotent.
void BrushSceneResolveMaterials(BrushScene *s);

// Release all resolved assets — material textures and models (call before
// dropping a scene).
void BrushSceneUnloadMaterials(BrushScene *s);

// Fill `out` with the submit-ready props for a block's material. False if
// the block has no material (or the name is unresolved) — submit plain.
bool BrushSceneBlockProps(const BrushScene *s, const BrushSceneBlock *k,
                          BrushMaterialProps *out);

// Resolve the scene's terrain_layer slots against the material library
// into submit-ready layer sets. Returns the layer count (contiguous from
// slot 0; the first unset/unresolved slot ends the list). Feed the result
// to BrushWorldSetLayers after load/reload/re-import.
int BrushSceneTerrainLayers(const BrushScene *s,
                            BrushTerrainLayer out[BRUSH_TERRAIN_LAYERS]);

// Resolve one material-library name to a terrain-layer set (albedo/normal/tile).
// Used for terrain layers and the independent road surface material. false if
// the name is empty/unknown or the material has no albedo.
bool BrushSceneMaterialLayer(const BrushScene *s, const char *name,
                             BrushTerrainLayer *out);

// Same, for a placed model's optional material (triplanar projection wraps
// the mesh — good for rocks/organic props). False = draw the model plain
// (its own embedded textures, if any).
bool BrushSceneModelProps(const BrushScene *s,
                          const BrushSceneModelInstance *m,
                          BrushMaterialProps *out);

// Fallback when a placed model has NO library material: build props from its
// OWN embedded normal map (glTF that ships a packed normal). Enables the
// UV-projected normal path so a self-textured asset lights with its packed
// detail + generated tangents; per-mesh diffuse is kept (albedo stays 0).
// False = the model has no embedded normal, so a plain draw is correct.
bool BrushSceneModelEmbeddedProps(const BrushSceneModelInstance *m,
                                  BrushMaterialProps *out);

// --- Persisted render settings ("post" lines) --------------------------------
// Apply the scene's saved tunables to the live render/post pipeline
// (unknown keys warn once). Call after Load/HotReload, once render is up.
void BrushSceneApplyRenderSettings(const BrushScene *s);

// Snapshot the live render/post tunables into the scene's post list so Save
// persists them (the editor calls this right before saving).
void BrushSceneCaptureRenderSettings(BrushScene *s);

#ifdef __cplusplus
}
#endif

#endif // B_SCENE_H
