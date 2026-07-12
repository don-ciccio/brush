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
  Texture2D albedoTex, normalTex, displacementTex, aoTex; // resolved, not saved
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

typedef struct BrushScene {
  Vector3 spawn;
  float timeHours; // starting clock (b_tod), <0 = not specified

  BrushSceneBlock blocks[BRUSH_SCENE_MAX_BLOCKS];
  int blockCount;
  BrushSceneLight lights[BRUSH_SCENE_MAX_LIGHTS];
  int lightCount;
  BrushSceneMaterial materials[BRUSH_SCENE_MAX_MATERIALS];
  int materialCount;
  BrushSceneModelInstance models[BRUSH_SCENE_MAX_MODELS];
  int modelCount;
  BrushScenePostSetting post[BRUSH_SCENE_MAX_POST];
  int postCount;
  // Paintable terrain layer slots -> material-library names ("" = unset).
  char terrainLayers[BRUSH_TERRAIN_LAYERS][BRUSH_SCENE_NAME_MAX];

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

// Same, for a placed model's optional material (triplanar projection wraps
// the mesh — good for rocks/organic props). False = draw the model plain
// (its own embedded textures, if any).
bool BrushSceneModelProps(const BrushScene *s,
                          const BrushSceneModelInstance *m,
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
