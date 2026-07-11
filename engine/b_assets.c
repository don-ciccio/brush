/*******************************************************************************************
 *   b_assets.c - Reference-counted asset registry
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_assets.h"

#include <stdlib.h>
#include <string.h>

#define BRUSH_ASSETS_MAX_TEXTURES 128
#define BRUSH_ASSETS_PATH_MAX 256

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
