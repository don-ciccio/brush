/*******************************************************************************************
 *   b_assets.c - Reference-counted asset registry + texture import cache
 *
 *   See b_assets.h and docs/asset-pipeline.md. Layout of this file:
 *     1. engine-root resolution (BrushEnginePath)
 *     2. import params (.import sidecars) + .ctex cook/load/validate
 *     3. cook worker thread (re-imports; first loads cook synchronously)
 *     4. the public registry API
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_assets.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rlgl.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define BRUSH_ASSETS_MAX_TEXTURES 128
#define BRUSH_ASSETS_PATH_MAX 256
#define BRUSH_CTEX_VERSION 1
#define BRUSH_CTEX_MAGIC 0x31585442u // 'BTX1'
#define BRUSH_IMPORT_DIR ".brush/imported"

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

// --- Import params (.import sidecars) ------------------------------------------

typedef struct TexImportParams {
  int maxSize;      // clamp the longest side (0 = no clamp)
  bool mipmaps;     // build the full mip chain offline
  bool isNormalMap; // reserved for the BC tier (affects the params hash)
  char compress[8]; // "none" today; "bc1"/"bc3" when the stb_dxt tier lands
} TexImportParams;

static void ImportParamsDefaults(TexImportParams *p, const char *srcPath) {
  p->maxSize = 2048;
  p->mipmaps = true;
  // Filename convention: *_normal* sources default to the normal-map
  // profile so the sidecar starts correct.
  const char *name = GetFileName(srcPath);
  p->isNormalMap = (name != NULL && strstr(name, "normal") != NULL);
  snprintf(p->compress, sizeof(p->compress), "none");
}

// Load <src>.import; if absent, write it with defaults (Godot-style: the
// sidecar IS the authored record of how this asset imports).
static void ImportParamsLoad(const char *srcPath, TexImportParams *p) {
  ImportParamsDefaults(p, srcPath);
  char path[BRUSH_ASSETS_PATH_MAX + 16];
  snprintf(path, sizeof(path), "%s.import", srcPath);

  FILE *f = fopen(path, "r");
  if (f == NULL) {
    f = fopen(path, "w");
    if (f != NULL) {
      fprintf(f, "[params]\n");
      fprintf(f, "max_size = %d\n", p->maxSize);
      fprintf(f, "generate_mipmaps = %s\n", p->mipmaps ? "true" : "false");
      fprintf(f, "compress = %s\n", p->compress);
      fprintf(f, "is_normal_map = %s\n", p->isNormalMap ? "true" : "false");
      fclose(f);
    }
    return;
  }
  char line[256], key[32], val[32];
  while (fgets(line, sizeof(line), f) != NULL) {
    if (sscanf(line, " %31[a-z_] = %31s", key, val) != 2) continue;
    if (strcmp(key, "max_size") == 0) p->maxSize = atoi(val);
    else if (strcmp(key, "generate_mipmaps") == 0) p->mipmaps = (strcmp(val, "true") == 0);
    else if (strcmp(key, "is_normal_map") == 0) p->isNormalMap = (strcmp(val, "true") == 0);
    else if (strcmp(key, "compress") == 0) snprintf(p->compress, sizeof(p->compress), "%s", val);
  }
  fclose(f);
  if (strcmp(p->compress, "none") != 0) {
    TraceLog(LOG_WARNING,
             "ASSETS: %s: compress '%s' not implemented yet (Tier 1) — "
             "cooking uncompressed",
             path, p->compress);
    snprintf(p->compress, sizeof(p->compress), "none");
  }
}

static uint32_t Fnv1a(const void *data, int len) {
  const unsigned char *b = data;
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

static uint32_t ImportParamsHash(const TexImportParams *p) {
  char canon[64];
  int n = snprintf(canon, sizeof(canon), "%d|%d|%d|%s", p->maxSize,
                   p->mipmaps ? 1 : 0, p->isNormalMap ? 1 : 0, p->compress);
  return Fnv1a(canon, n);
}

// --- .ctex container ------------------------------------------------------------

typedef struct CtexHeader {
  uint32_t magic;           // BRUSH_CTEX_MAGIC
  uint32_t importerVersion; // BRUSH_CTEX_VERSION — bump to invalidate all
  uint64_t srcSize;         // source file identity at cook time
  uint64_t srcMtime;
  uint32_t paramsHash; // ImportParamsHash at cook time
  uint32_t format;     // raylib PixelFormat
  uint16_t width, height, mipCount, reserved;
} CtexHeader;

static bool StatFile(const char *path, uint64_t *size, uint64_t *mtime) {
  struct stat st;
  if (stat(path, &st) != 0) return false;
  *size = (uint64_t)st.st_size;
  *mtime = (uint64_t)st.st_mtime;
  return true;
}

static void CtexPath(const char *srcPath, char *out, int cap) {
  snprintf(out, (size_t)cap, BRUSH_IMPORT_DIR "/%s.ctex", srcPath);
}

// mkdir -p for the parent directory of `filePath`.
static void MakeParentDirs(const char *filePath) {
  char buf[PATH_MAX];
  snprintf(buf, sizeof(buf), "%s", filePath);
  char *last = strrchr(buf, '/');
  if (last == NULL) return;
  *last = '\0';
  for (char *p = buf + 1; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
    mkdir(buf, 0755);
    *p = '/';
  }
  mkdir(buf, 0755);
}

// Total byte size of a tightly-packed mip chain (raylib's Image layout
// after ImageMipmaps: mip0 first, each level directly after the previous).
static int MipChainSize(int w, int h, int format, int mips) {
  int total = 0;
  for (int i = 0; i < mips; i++) {
    total += GetPixelDataSize(w, h, format);
    if (w > 1) w /= 2;
    if (h > 1) h /= 2;
  }
  return total;
}

// Cook one source image into a .ctex. Pure CPU — safe on the worker.
static bool CookTexture(const char *srcPath, const char *dstPath,
                        const TexImportParams *prm) {
  double t0 = GetTime();
  Image img = LoadImage(srcPath);
  if (img.data == NULL) return false;
  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  if (prm->maxSize > 0 &&
      (img.width > prm->maxSize || img.height > prm->maxSize)) {
    float s = (float)prm->maxSize /
              (float)(img.width > img.height ? img.width : img.height);
    int nw = (int)(img.width * s), nh = (int)(img.height * s);
    ImageResize(&img, nw > 1 ? nw : 1, nh > 1 ? nh : 1);
  }
  if (prm->mipmaps) ImageMipmaps(&img);

  CtexHeader h = {0};
  h.magic = BRUSH_CTEX_MAGIC;
  h.importerVersion = BRUSH_CTEX_VERSION;
  if (!StatFile(srcPath, &h.srcSize, &h.srcMtime)) {
    UnloadImage(img);
    return false;
  }
  h.paramsHash = ImportParamsHash(prm);
  h.format = (uint32_t)img.format;
  h.width = (uint16_t)img.width;
  h.height = (uint16_t)img.height;
  h.mipCount = (uint16_t)img.mipmaps;

  MakeParentDirs(dstPath);
  FILE *f = fopen(dstPath, "wb");
  if (f == NULL) {
    UnloadImage(img);
    return false;
  }
  int dataSize = MipChainSize(img.width, img.height, img.format, img.mipmaps);
  bool ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
            fwrite(img.data, (size_t)dataSize, 1, f) == 1;
  fclose(f);
  UnloadImage(img);
  if (ok)
    TraceLog(LOG_INFO, "ASSETS: imported %s (%dx%d, %d mips, %.0f ms)",
             srcPath, h.width, h.height, h.mipCount,
             (GetTime() - t0) * 1000.0);
  return ok;
}

// Is the cooked file still a faithful product of source + params + importer?
static bool CtexValid(const char *srcPath, const char *ctexPath,
                      const TexImportParams *prm) {
  FILE *f = fopen(ctexPath, "rb");
  if (f == NULL) return false;
  CtexHeader h;
  bool ok = fread(&h, sizeof(h), 1, f) == 1;
  fclose(f);
  if (!ok || h.magic != BRUSH_CTEX_MAGIC ||
      h.importerVersion != BRUSH_CTEX_VERSION)
    return false;
  uint64_t size, mtime;
  if (!StatFile(srcPath, &size, &mtime)) return false;
  return h.srcSize == size && h.srcMtime == mtime &&
         h.paramsHash == ImportParamsHash(prm);
}

// GPU-upload a .ctex: read header, pipe the mip chain straight to GL.
// MAIN THREAD only.
static Texture2D LoadCtex(const char *ctexPath) {
  Texture2D tex = {0};
  FILE *f = fopen(ctexPath, "rb");
  if (f == NULL) return tex;
  CtexHeader h;
  if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != BRUSH_CTEX_MAGIC) {
    fclose(f);
    return tex;
  }
  int dataSize = MipChainSize(h.width, h.height, (int)h.format, h.mipCount);
  void *data = malloc((size_t)dataSize);
  if (data == NULL || fread(data, (size_t)dataSize, 1, f) != 1) {
    free(data);
    fclose(f);
    return tex;
  }
  fclose(f);
  tex.id = rlLoadTexture(data, h.width, h.height, (int)h.format, h.mipCount);
  free(data);
  if (tex.id == 0) return (Texture2D){0};
  tex.width = h.width;
  tex.height = h.height;
  tex.mipmaps = h.mipCount;
  tex.format = (int)h.format;
  SetTextureFilter(tex, tex.mipmaps > 1 ? TEXTURE_FILTER_TRILINEAR
                                        : TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(tex, TEXTURE_WRAP_REPEAT);
  return tex;
}

static bool IsCookable(const char *path) {
  return IsFileExtension(path, ".png") || IsFileExtension(path, ".jpg") ||
         IsFileExtension(path, ".jpeg") || IsFileExtension(path, ".tga") ||
         IsFileExtension(path, ".bmp");
}

// --- Cook worker (background re-imports) -----------------------------------------

typedef enum { JOB_EMPTY = 0, JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAILED } JobState;

typedef struct CookJob {
  char src[BRUSH_ASSETS_PATH_MAX];
  char dst[BRUSH_ASSETS_PATH_MAX + 32];
  TexImportParams params;
  JobState state;
} CookJob;

#define MAX_COOK_JOBS 16
static CookJob g_jobs[MAX_COOK_JOBS];
static pthread_mutex_t g_jobMx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_jobCv = PTHREAD_COND_INITIALIZER;
static pthread_t g_cookThread;
static bool g_cookThreadUp = false;
static bool g_cookQuit = false;

static void *CookWorker(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&g_jobMx);
    CookJob *job = NULL;
    while (job == NULL && !g_cookQuit) {
      for (int i = 0; i < MAX_COOK_JOBS; i++)
        if (g_jobs[i].state == JOB_QUEUED) { job = &g_jobs[i]; break; }
      if (job == NULL) pthread_cond_wait(&g_jobCv, &g_jobMx);
    }
    if (g_cookQuit) {
      pthread_mutex_unlock(&g_jobMx);
      return NULL;
    }
    job->state = JOB_RUNNING;
    pthread_mutex_unlock(&g_jobMx);

    bool ok = CookTexture(job->src, job->dst, &job->params);

    pthread_mutex_lock(&g_jobMx);
    job->state = ok ? JOB_DONE : JOB_FAILED;
    pthread_mutex_unlock(&g_jobMx);
  }
}

static void QueueCook(const char *src, const char *dst,
                      const TexImportParams *prm) {
  pthread_mutex_lock(&g_jobMx);
  if (!g_cookThreadUp) {
    g_cookQuit = false;
    if (pthread_create(&g_cookThread, NULL, CookWorker, NULL) == 0)
      g_cookThreadUp = true;
  }
  for (int i = 0; i < MAX_COOK_JOBS; i++) {
    if (g_jobs[i].state != JOB_EMPTY) continue;
    snprintf(g_jobs[i].src, sizeof(g_jobs[i].src), "%s", src);
    snprintf(g_jobs[i].dst, sizeof(g_jobs[i].dst), "%s", dst);
    g_jobs[i].params = *prm;
    g_jobs[i].state = JOB_QUEUED;
    pthread_cond_signal(&g_jobCv);
    break;
  }
  pthread_mutex_unlock(&g_jobMx);
}

// --- Registry --------------------------------------------------------------------

typedef struct TexEntry {
  char path[BRUSH_ASSETS_PATH_MAX];
  Texture2D tex; // id 0 = load failed (negative cache: warn once, not per frame)
  int refs;
  bool watch;         // cookable source on disk — eligible for re-import
  bool cookPending;   // a worker job is in flight for this source
  uint64_t srcSize, srcMtime; // last identity we imported/saw
  uint64_t impSize, impMtime; // .import sidecar identity (0 if absent)
} TexEntry;

static void StatSourcePair(TexEntry *e) {
  StatFile(e->path, &e->srcSize, &e->srcMtime);
  char imp[BRUSH_ASSETS_PATH_MAX + 16];
  snprintf(imp, sizeof(imp), "%s.import", e->path);
  e->impSize = e->impMtime = 0;
  StatFile(imp, &e->impSize, &e->impMtime);
}

static TexEntry g_tex[BRUSH_ASSETS_MAX_TEXTURES];
static int g_texCount = 0;

// Cache-first load: valid .ctex -> fast path; else cook now (first import)
// then load; any failure -> raw LoadTexture fallback.
static Texture2D AcquireTexture(TexEntry *e) {
  const char *path = e->path;
  if (IsCookable(path) && FileExists(path)) {
    TexImportParams prm;
    ImportParamsLoad(path, &prm);
    char ctex[BRUSH_ASSETS_PATH_MAX + 32];
    CtexPath(path, ctex, sizeof(ctex));
    if (!CtexValid(path, ctex, &prm)) CookTexture(path, ctex, &prm);
    Texture2D t = LoadCtex(ctex);
    if (t.id != 0) {
      e->watch = true;
      StatSourcePair(e);
      return t;
    }
  }
  // Raw fallback (non-cookable formats, cook/IO failure).
  Texture2D t = LoadTexture(path);
  if (t.id != 0) {
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
  }
  return t;
}

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
  memset(e, 0, sizeof(*e));
  strncpy(e->path, path, sizeof(e->path) - 1);
  e->refs = 1;
  e->tex = AcquireTexture(e);
  if (e->tex.id == 0)
    TraceLog(LOG_WARNING, "ASSETS: missing texture %s", path);
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

bool BrushAssetsUpdate(void) {
  bool changed = false;

  // 1. Land finished worker cooks: GPU-load the fresh .ctex and swap it
  //    into the entry (same path, same refcount, new texture).
  char doneSrc[MAX_COOK_JOBS][BRUSH_ASSETS_PATH_MAX];
  char doneDst[MAX_COOK_JOBS][BRUSH_ASSETS_PATH_MAX + 32];
  bool doneOk[MAX_COOK_JOBS];
  int doneCount = 0;
  pthread_mutex_lock(&g_jobMx);
  for (int i = 0; i < MAX_COOK_JOBS; i++) {
    if (g_jobs[i].state != JOB_DONE && g_jobs[i].state != JOB_FAILED) continue;
    snprintf(doneSrc[doneCount], sizeof(doneSrc[0]), "%s", g_jobs[i].src);
    snprintf(doneDst[doneCount], sizeof(doneDst[0]), "%s", g_jobs[i].dst);
    doneOk[doneCount++] = (g_jobs[i].state == JOB_DONE);
    g_jobs[i].state = JOB_EMPTY;
  }
  pthread_mutex_unlock(&g_jobMx);

  for (int d = 0; d < doneCount; d++) {
    for (int i = 0; i < g_texCount; i++) {
      TexEntry *e = &g_tex[i];
      if (strcmp(e->path, doneSrc[d]) != 0) continue;
      e->cookPending = false;
      if (doneOk[d]) {
        Texture2D fresh = LoadCtex(doneDst[d]);
        if (fresh.id == 0)
          TraceLog(LOG_WARNING, "ASSETS: re-import upload failed for %s",
                   doneSrc[d]);
        if (fresh.id != 0) {
          if (e->tex.id != 0) UnloadTexture(e->tex);
          e->tex = fresh;
          StatSourcePair(e);
          changed = true;
          TraceLog(LOG_INFO, "ASSETS: re-imported %s", e->path);
        }
      }
      break;
    }
  }

  // 2. Watch sources for edits. No internal throttle: callers already pick
  //    the cadence (the sandbox polls on the same 30-step clock as scene
  //    hot reload; the editor calls per frame — a handful of stats).
  {
    for (int i = 0; i < g_texCount; i++) {
      TexEntry *e = &g_tex[i];
      if (!e->watch || e->cookPending) continue;
      uint64_t size, mtime;
      if (!StatFile(e->path, &size, &mtime)) continue;
      uint64_t impSize = 0, impMtime = 0;
      char imp[BRUSH_ASSETS_PATH_MAX + 16];
      snprintf(imp, sizeof(imp), "%s.import", e->path);
      StatFile(imp, &impSize, &impMtime);
      if (size == e->srcSize && mtime == e->srcMtime &&
          impSize == e->impSize && impMtime == e->impMtime)
        continue;
      TexImportParams prm;
      ImportParamsLoad(e->path, &prm);
      char ctex[BRUSH_ASSETS_PATH_MAX + 32];
      CtexPath(e->path, ctex, sizeof(ctex));
      e->cookPending = true;
      TraceLog(LOG_DEBUG, "ASSETS: source changed, cooking %s", e->path);
      QueueCook(e->path, ctex, &prm);
    }
  }
  return changed;
}

void BrushAssetsShutdown(void) {
  if (g_cookThreadUp) {
    pthread_mutex_lock(&g_jobMx);
    g_cookQuit = true;
    pthread_cond_broadcast(&g_jobCv);
    pthread_mutex_unlock(&g_jobMx);
    pthread_join(g_cookThread, NULL);
    g_cookThreadUp = false;
  }
  for (int i = 0; i < g_texCount; i++)
    if (g_tex[i].tex.id != 0) UnloadTexture(g_tex[i].tex);
  g_texCount = 0;
}
