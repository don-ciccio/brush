/*******************************************************************************************
 *   b_assets.c - Reference-counted asset registry
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_assets.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define BRUSH_ASSETS_MAX_TEXTURES 128
#define BRUSH_ASSETS_PATH_MAX 256

// --- Engine-root resolution ---------------------------------------------------
// The engine root is the directory that contains engine/shaders — found by
// walking up from the executable (handles build/, build/Brush.app/..., and
// any future install layout). Resolved once.
static const char *EngineRoot(void) {
  static char root[PATH_MAX];
  static bool inited = false;
  if (inited) return root;
  inited = true;
  root[0] = '\0';

  char exe[PATH_MAX] = {0};
#if defined(__APPLE__)
  uint32_t sz = sizeof(exe);
  if (_NSGetExecutablePath(exe, &sz) != 0) exe[0] = '\0';
#elif defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) exe[n] = '\0';
#endif

  char real[PATH_MAX];
  if (exe[0] != '\0' && realpath(exe, real) != NULL) {
    // Start at the binary's directory, walk up looking for engine/shaders.
    char *slash = strrchr(real, '/');
    if (slash != NULL) *slash = '\0';
    for (int up = 0; up < 6 && real[0] != '\0'; up++) {
      char probe[PATH_MAX + 64];
      snprintf(probe, sizeof(probe), "%s/engine/shaders/lit.fs", real);
      if (access(probe, R_OK) == 0) {
        snprintf(root, sizeof(root), "%s", real);
        break;
      }
      slash = strrchr(real, '/');
      if (slash == NULL) break;
      *slash = '\0';
    }
  }
  if (root[0] == '\0') {
    // Fallback: cwd (running from the repo root, the pre-project behavior).
    snprintf(root, sizeof(root), ".");
  }
  TraceLog(LOG_INFO, "ASSETS: engine root = %s", root);
  return root;
}

const char *BrushEnginePath(const char *relative) {
  static char ring[4][PATH_MAX + 64];
  static int idx = 0;
  char *out = ring[idx];
  idx = (idx + 1) & 3;
  snprintf(out, sizeof(ring[0]), "%s/%s", EngineRoot(), relative);
  return out;
}

typedef struct TexEntry {
  char path[BRUSH_ASSETS_PATH_MAX];
  Texture2D tex; // id 0 = load failed (negative cache: warn once, not per frame)
  int refs;
} TexEntry;

static TexEntry g_tex[BRUSH_ASSETS_MAX_TEXTURES];
static int g_texCount = 0;

Texture2D BrushAssetsTexture(const char *path) {
  if (path == NULL || path[0] == '\0') return (Texture2D){0};

  for (int i = 0; i < g_texCount; i++) {
    if (strcmp(g_tex[i].path, path) == 0) {
      g_tex[i].refs++;
      return g_tex[i].tex;
    }
  }

  if (g_texCount >= BRUSH_ASSETS_MAX_TEXTURES) {
    TraceLog(LOG_WARNING, "ASSETS: texture cache full (%d), can't load %s",
             BRUSH_ASSETS_MAX_TEXTURES, path);
    return (Texture2D){0};
  }

  TexEntry *e = &g_tex[g_texCount++];
  strncpy(e->path, path, sizeof(e->path) - 1);
  e->path[sizeof(e->path) - 1] = '\0';
  e->refs = 1;
  e->tex = LoadTexture(path);
  if (e->tex.id == 0) {
    TraceLog(LOG_WARNING, "ASSETS: missing texture %s", path);
    return e->tex;
  }
  // Full quality treatment once, so no call site re-derives it.
  GenTextureMipmaps(&e->tex);
  SetTextureFilter(e->tex, TEXTURE_FILTER_TRILINEAR);
  SetTextureWrap(e->tex, TEXTURE_WRAP_REPEAT);
  TraceLog(LOG_INFO, "ASSETS: loaded texture %s (%dx%d)", path, e->tex.width,
           e->tex.height);
  return e->tex;
}

void BrushAssetsReleaseTexture(Texture2D tex) {
  if (tex.id == 0) return;
  for (int i = 0; i < g_texCount; i++) {
    if (g_tex[i].tex.id != tex.id) continue;
    if (--g_tex[i].refs > 0) return;
    UnloadTexture(g_tex[i].tex);
    TraceLog(LOG_DEBUG, "ASSETS: unloaded texture %s", g_tex[i].path);
    g_tex[i] = g_tex[--g_texCount]; // swap-remove
    return;
  }
}

void BrushAssetsShutdown(void) {
  for (int i = 0; i < g_texCount; i++)
    if (g_tex[i].tex.id != 0) UnloadTexture(g_tex[i].tex);
  g_texCount = 0;
}
