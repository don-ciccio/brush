/*******************************************************************************************
 *   b_scene.c - World definition files (see b_scene.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <raymath.h>

bool BrushSceneLoad(BrushScene *s, const char *path) {
  BrushScene temp = {0};
  temp.timeHours = -1.0f;
  strncpy(temp.path, path, BRUSH_SCENE_PATH_MAX - 1);
  temp.path[BRUSH_SCENE_PATH_MAX - 1] = '\0';

  FILE *f = fopen(path, "r");
  if (f == NULL) return false;

  char line[512];
  int lineNo = 0;
  while (fgets(line, sizeof(line), f) != NULL) {
    lineNo++;
    // Strip leading whitespace; skip blanks and comments.
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '\n' || *p == '#') continue;

    int version;
    float x, y, z, rx, ry, rz, sx, sy, sz, r, g, b, radius;
    int ir, ig, ib, flicker;

    if (sscanf(p, "version %d", &version) == 1) {
      if (version != 1)
        TraceLog(LOG_WARNING, "BRUSH scene: %s is version %d (engine reads 1)",
                 path, version);
    } else if (sscanf(p, "spawn %f %f %f", &x, &y, &z) == 3) {
      temp.spawn = (Vector3){x, y, z};
    } else if (sscanf(p, "time %f", &x) == 1) {
      temp.timeHours = x;
    } else if (sscanf(p, "block %f %f %f %f %f %f %f %f %f %d %d %d", &x, &y, &z, &rx, &ry, &rz, &sx, &sy, &sz, &ir, &ig, &ib) == 12) {
      if (temp.blockCount < BRUSH_SCENE_MAX_BLOCKS) {
        temp.blocks[temp.blockCount++] = (BrushSceneBlock){
            .pos = {x, y, z},
            .size = {sx, sy, sz},
            .rot = {rx, ry, rz},
            .color = {(unsigned char)ir, (unsigned char)ig, (unsigned char)ib, 255},
        };
      }
    } else if (sscanf(p, "block %f %f %f %f %f %f %d %d %d", &x, &y, &z, &sx, &sy, &sz, &ir, &ig, &ib) == 9) {
      if (temp.blockCount < BRUSH_SCENE_MAX_BLOCKS) {
        temp.blocks[temp.blockCount++] = (BrushSceneBlock){
            .pos = {x, y, z},
            .size = {sx, sy, sz},
            .rot = {0, 0, 0},
            .color = {(unsigned char)ir, (unsigned char)ig, (unsigned char)ib, 255},
        };
      }
    } else if (sscanf(p, "light %f %f %f %f %f %f %f %d", &x, &y, &z, &r, &g,
                      &b, &radius, &flicker) == 8) {
      if (temp.lightCount < BRUSH_SCENE_MAX_LIGHTS) {
        temp.lights[temp.lightCount++] = (BrushSceneLight){
            .light = {.position = {x, y, z},
                      .color = {r, g, b},
                      .radius = radius},
            .flicker = (flicker != 0),
        };
      }
    } else {
      // Forward compatibility: unknown entity types are skipped, not fatal.
      TraceLog(LOG_WARNING, "BRUSH scene: %s:%d unknown line: %.40s", path,
               lineNo, p);
    }
  }
  fclose(f);

  temp.modTime = GetFileModTime(path);
  *s = temp;
  TraceLog(LOG_INFO, "BRUSH scene: loaded %s (%d blocks, %d lights)", path,
           s->blockCount, s->lightCount);
  return true;
}

bool BrushSceneSave(BrushScene *s, const char *path) {
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    TraceLog(LOG_WARNING, "BRUSH scene: cannot write %s", path);
    return false;
  }
  fprintf(f, "# brush world definition (see engine/b_scene.h)\n");
  fprintf(f, "version 1\n\n");
  fprintf(f, "spawn %g %g %g\n", s->spawn.x, s->spawn.y, s->spawn.z);
  if (s->timeHours >= 0.0f) fprintf(f, "time %g\n", s->timeHours);
  fprintf(f, "\n# block  x y z  rx ry rz  sx sy sz  r g b\n");
  for (int i = 0; i < s->blockCount; i++) {
    const BrushSceneBlock *k = &s->blocks[i];
    fprintf(f, "block %g %g %g  %g %g %g  %g %g %g  %d %d %d\n", k->pos.x, k->pos.y,
            k->pos.z, k->rot.x, k->rot.y, k->rot.z, k->size.x, k->size.y, k->size.z, 
            k->color.r, k->color.g, k->color.b);
  }
  fprintf(f, "\n# light  x y z  r g b (linear)  radius  flicker\n");
  for (int i = 0; i < s->lightCount; i++) {
    const BrushSceneLight *l = &s->lights[i];
    fprintf(f, "light %g %g %g  %g %g %g  %g %d\n", l->light.position.x,
            l->light.position.y, l->light.position.z, l->light.color.x,
            l->light.color.y, l->light.color.z, l->light.radius,
            l->flicker ? 1 : 0);
  }
  fclose(f);
  strncpy(s->path, path, BRUSH_SCENE_PATH_MAX - 1);
  s->modTime = GetFileModTime(path);
  TraceLog(LOG_INFO, "BRUSH scene: saved %s (%d blocks, %d lights)", path,
           s->blockCount, s->lightCount);
  return true;
}

bool BrushSceneHotReload(BrushScene *s) {
  if (s->path[0] == '\0') return false;
  long mt = GetFileModTime(s->path);
  if (getenv("BRUSH_SCENE_DBG") != NULL)
    TraceLog(LOG_INFO, "SCENEDBG path=%s mt=%ld have=%ld", s->path, mt,
             s->modTime);
  if (mt == s->modTime || mt == 0) return false;
  char path[BRUSH_SCENE_PATH_MAX];
  strncpy(path, s->path, sizeof(path));
  path[sizeof(path) - 1] = '\0';
  if (!BrushSceneLoad(s, path)) return false;
  TraceLog(LOG_INFO, "BRUSH scene: hot-reloaded %s", path);
  return true;
}

// Euler (degrees) -> rotation matrix, composed X then Y then Z in raylib's
// row-vector convention. This is EXACTLY ImGuizmo's Recompose/Decompose
// order — the editor gizmo, this render matrix, and the physics collider
// (QuaternionFromMatrix of the same rotation in b_physics) must all agree
// or blocks visibly jump when a gizmo drag ends.
Matrix BrushEulerXYZ(Vector3 degrees) {
  Matrix r = MatrixMultiply(MatrixRotateX(degrees.x * DEG2RAD),
                            MatrixRotateY(degrees.y * DEG2RAD));
  return MatrixMultiply(r, MatrixRotateZ(degrees.z * DEG2RAD));
}

Matrix BrushBlockGetModelMatrix(const BrushSceneBlock *k) {
  Matrix xf = MatrixScale(k->size.x, k->size.y, k->size.z);
  xf = MatrixMultiply(xf, BrushEulerXYZ(k->rot));
  return MatrixMultiply(xf, MatrixTranslate(k->pos.x, k->pos.y, k->pos.z));
}
