/*******************************************************************************************
 *   b_project.h - Project definition (project.def)
 *
 *   A brush PROJECT is a folder with a project.def at its root — the unit
 *   the Project Manager screen creates/opens and the player runs. Same
 *   plain-text warn-and-skip format family as world.def:
 *
 *     version 1
 *     name My Game
 *     scene assets/main.def
 *
 *   `scene` is the world.def the player boots and the editor opens; every
 *   path inside a project is project-root-relative, and the editor/player
 *   chdir() into the project so those paths resolve unchanged. Engine files
 *   (shaders, lookup textures) resolve via BrushEnginePath instead — see
 *   docs/asset-pipeline.md for the full picture.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_PROJECT_H
#define B_PROJECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define BRUSH_PROJECT_NAME_MAX 64
#define BRUSH_PROJECT_SCENE_MAX 256

typedef struct BrushProject {
  char name[BRUSH_PROJECT_NAME_MAX];
  char scene[BRUSH_PROJECT_SCENE_MAX]; // project-relative main scene path
} BrushProject;

// Read `dir`/project.def. False if missing/unreadable (out gets defaults:
// name "Untitled", scene "assets/main.def").
bool BrushProjectLoad(BrushProject *p, const char *dir);

// Write `dir`/project.def.
bool BrushProjectSave(const BrushProject *p, const char *dir);

// chdir into the project selected by --project <dir> argv or the
// BRUSH_PROJECT env (argv wins), then load its project.def into `p`.
// With neither given, stays in the cwd and tries ./project.def (the engine
// repo is itself a project). Returns true if a project.def was loaded.
bool BrushProjectBoot(BrushProject *p, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif // B_PROJECT_H
