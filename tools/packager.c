/*******************************************************************************************
 *   packager.c - Project packager (docs/asset-pipeline.md, Phase 3)
 *
 *   Packs one project into a single game.pak archive:
 *
 *     packager [projectDir]                cook + write <projectDir>/game.pak
 *     packager [projectDir] --release DIR  also stage a runnable release
 *                                          folder: player binary, engine
 *                                          shaders/resources, project.def,
 *                                          game.pak — nothing else needed.
 *
 *   What goes in: project.def, everything under assets/ EXCEPT source
 *   images that have a cooked .ctex (the cooked form ships instead) and
 *   .import sidecars (cook-time inputs, not runtime data), plus the whole
 *   .brush/imported/ cache. Textures are re-cooked first (CookTree), so
 *   the pak is always current.
 *
 *   Format (read by BrushAssetsMount):
 *     [PakHeader: magic 'BPK1' u32 | version u32 | indexOffset u64]
 *     [file bytes, each entry 16-byte aligned]
 *     [index: count u32 | pathBytes u32 |
 *      count * { hash u64 | offset u64 | size u64 | pathOffset u32 } |
 *      path blob (NUL-separated)]   — entries sorted by FNV-1a64 hash
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "brush.h"

#include <raylib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PAK_FILES 2048

typedef struct Item {
  char path[512];
  uint64_t hash, offset, size;
  uint32_t pathOffset;
} Item;

static Item g_items[MAX_PAK_FILES];
static int g_itemCount = 0;

static uint64_t Fnv1a64(const char *s) {
  uint64_t h = 14695981039346656037ull;
  while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211ull;
  return h;
}

static void AddItem(const char *path) {
  if (g_itemCount >= MAX_PAK_FILES) {
    TraceLog(LOG_WARNING, "PACK: too many files, skipping %s", path);
    return;
  }
  Item *it = &g_items[g_itemCount++];
  memset(it, 0, sizeof(*it));
  snprintf(it->path, sizeof(it->path), "%s", path);
  it->hash = Fnv1a64(path);
}

static int CompareItems(const void *a, const void *b) {
  uint64_t ha = ((const Item *)a)->hash, hb = ((const Item *)b)->hash;
  return (ha > hb) - (ha < hb);
}

// Everything under assets/ except cook inputs already represented by their
// cooked form (raw image + .import sidecar).
static void CollectAssets(void) {
  FilePathList fl = LoadDirectoryFilesEx("assets", NULL, true);
  for (unsigned int i = 0; i < fl.count; i++) {
    const char *p = fl.paths[i];
    const char *name = GetFileName(p);
    if (name != NULL && name[0] == '.') continue; // .DS_Store etc.
    if (IsFileExtension(p, ".import")) continue;
    char ctex[600];
    snprintf(ctex, sizeof(ctex), ".brush/imported/%s.ctex", p);
    if (FileExists(ctex)) continue; // cooked form ships instead
    AddItem(p);
  }
  UnloadDirectoryFiles(fl);

  FilePathList cl = LoadDirectoryFilesEx(".brush/imported", NULL, true);
  for (unsigned int i = 0; i < cl.count; i++)
    if (IsFileExtension(cl.paths[i], ".ctex")) AddItem(cl.paths[i]);
  UnloadDirectoryFiles(cl);

  if (FileExists("project.def")) AddItem("project.def");
}

static bool WritePak(const char *outPath) {
  FILE *out = fopen(outPath, "wb");
  if (out == NULL) {
    TraceLog(LOG_ERROR, "PACK: cannot write %s", outPath);
    return false;
  }
  // Header placeholder; indexOffset patched at the end.
  uint32_t magic = 0x314B5042u, version = 1;
  uint64_t indexOffset = 0;
  fwrite(&magic, 4, 1, out);
  fwrite(&version, 4, 1, out);
  fwrite(&indexOffset, 8, 1, out);

  qsort(g_items, (size_t)g_itemCount, sizeof(Item), CompareItems);

  uint64_t total = 0;
  for (int i = 0; i < g_itemCount; i++) {
    // 16-byte alignment for direct/mmap-friendly reads.
    long pos = ftell(out);
    while (pos % 16 != 0) { fputc(0, out); pos++; }
    g_items[i].offset = (uint64_t)pos;

    int size = 0;
    unsigned char *data = LoadFileData(g_items[i].path, &size);
    if (data == NULL) {
      TraceLog(LOG_WARNING, "PACK: unreadable, skipped: %s", g_items[i].path);
      g_items[i].size = 0;
      continue;
    }
    fwrite(data, (size_t)size, 1, out);
    g_items[i].size = (uint64_t)size;
    total += (uint64_t)size;
    UnloadFileData(data);
  }

  // Index: entries then the NUL-separated path blob.
  indexOffset = (uint64_t)ftell(out);
  uint32_t count = (uint32_t)g_itemCount, pathBytes = 0;
  for (int i = 0; i < g_itemCount; i++) {
    g_items[i].pathOffset = pathBytes;
    pathBytes += (uint32_t)strlen(g_items[i].path) + 1;
  }
  fwrite(&count, 4, 1, out);
  fwrite(&pathBytes, 4, 1, out);
  for (int i = 0; i < g_itemCount; i++) {
    fwrite(&g_items[i].hash, 8, 1, out);
    fwrite(&g_items[i].offset, 8, 1, out);
    fwrite(&g_items[i].size, 8, 1, out);
    fwrite(&g_items[i].pathOffset, 4, 1, out);
  }
  for (int i = 0; i < g_itemCount; i++)
    fwrite(g_items[i].path, strlen(g_items[i].path) + 1, 1, out);

  fseek(out, 8, SEEK_SET); // patch header
  fwrite(&indexOffset, 8, 1, out);
  fclose(out);
  TraceLog(LOG_INFO, "PACK: %s — %d files, %.1f MB data", outPath,
           g_itemCount, (double)total / (1024.0 * 1024.0));
  return true;
}

// mkdir -p
static void MakeDirs(const char *path) {
  char buf[600];
  snprintf(buf, sizeof(buf), "%s", path);
  for (char *p = buf + 1; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
    mkdir(buf, 0755);
    *p = '/';
  }
  mkdir(buf, 0755);
}

static bool CopyFileRaw(const char *src, const char *dst) {
  int size = 0;
  unsigned char *data = LoadFileData(src, &size);
  if (data == NULL) return false;
  bool ok = SaveFileData(dst, data, size);
  UnloadFileData(data);
  return ok;
}

static void CopyTree(const char *srcDir, const char *dstDir) {
  char src[600];
  snprintf(src, sizeof(src), "%s", srcDir); // srcDir may be in the
                                            // BrushEnginePath ring buffer
  FilePathList fl = LoadDirectoryFilesEx(src, NULL, true);
  for (unsigned int i = 0; i < fl.count; i++) {
    const char *rel = fl.paths[i] + strlen(src);
    while (*rel == '/') rel++;
    char dst[1200];
    snprintf(dst, sizeof(dst), "%s/%s", dstDir, rel);
    char parent[1200];
    snprintf(parent, sizeof(parent), "%s", dst);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = '\0'; MakeDirs(parent); }
    CopyFileRaw(fl.paths[i], dst);
  }
  UnloadDirectoryFiles(fl);
}

// Stage a runnable folder: player + engine files + project.def + pak. The
// player finds its engine root by walking up from the executable, so
// placing engine/ next to the binary "just works".
static void StageRelease(const char *dir, const char *pakPath,
                         const BrushProject *project) {
  MakeDirs(dir);
  char dst[700];

  snprintf(dst, sizeof(dst), "%s/%s", dir,
           project->name[0] ? project->name : "game");
  CopyFileRaw(BrushEnginePath("build/sandbox"), dst);
  chmod(dst, 0755);

  snprintf(dst, sizeof(dst), "%s/engine/shaders", dir);
  CopyTree(BrushEnginePath("engine/shaders"), dst);
  snprintf(dst, sizeof(dst), "%s/engine/resources", dir);
  CopyTree(BrushEnginePath("engine/resources"), dst);

  snprintf(dst, sizeof(dst), "%s/project.def", dir);
  CopyFileRaw("project.def", dst);
  snprintf(dst, sizeof(dst), "%s/game.pak", dir);
  CopyFileRaw(pakPath, dst);

  TraceLog(LOG_INFO, "PACK: release staged at %s (run ./%s from there)", dir,
           project->name[0] ? project->name : "game");
}

int main(int argc, char **argv) {
  const char *projectDir = ".";
  const char *releaseDir = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--release") == 0 && i + 1 < argc)
      releaseDir = argv[++i];
    else projectDir = argv[i];
  }
  if (chdir(projectDir) != 0) {
    TraceLog(LOG_ERROR, "PACK: cannot enter %s", projectDir);
    return 1;
  }

  BrushProject project;
  if (!BrushProjectLoad(&project, "."))
    TraceLog(LOG_WARNING, "PACK: no project.def here — packing anyway");
  TraceLog(LOG_INFO, "PACK: project '%s' (scene %s)", project.name,
           project.scene);

  int cooked = BrushAssetsCookTree("assets");
  if (cooked > 0) TraceLog(LOG_INFO, "PACK: cooked %d texture(s)", cooked);

  CollectAssets();
  if (!WritePak("game.pak")) return 1;

  if (releaseDir != NULL) StageRelease(releaseDir, "game.pak", &project);
  return 0;
}
