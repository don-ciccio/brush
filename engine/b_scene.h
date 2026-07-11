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
 *     version 1
 *     spawn 0 0.5 8
 *     time 10.5
 *     block  <x y z> <sx sy sz> <r g b>
 *     light  <x y z> <r g b (linear, may exceed 1)> <radius> <flicker 0|1>
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
#define BRUSH_SCENE_PATH_MAX 512

typedef struct BrushSceneBlock {
  Vector3 pos, size;
  Vector3 rot; // Euler angles in degrees (pitch, yaw, roll)
  Color color;
} BrushSceneBlock;

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

// Re-load if the file changed on disk since the last Load/HotReload. Returns
// true when a reload happened (the game should re-apply colliders etc.).
bool BrushSceneHotReload(BrushScene *s);

#ifdef __cplusplus
}
#endif

#endif // B_SCENE_H
