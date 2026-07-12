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

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rlgl.h>
#include <raymath.h>

#include "b_physics.h" // model collision-shape cache (cook + JPH_Shape refs)

#define STB_DXT_IMPLEMENTATION
#define STB_DXT_STATIC
#include "../external/stb/stb_dxt.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define BRUSH_ASSETS_MAX_TEXTURES 128
#define BRUSH_ASSETS_PATH_MAX 256
#define BRUSH_CTEX_VERSION 2
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

static void ImportParamsDefaults(BrushTexImportParams *p, const char *srcPath) {
  p->maxSize = 2048;
  p->mipmaps = true;
  // Filename convention: *_normal* sources default to the normal-map
  // profile so the sidecar starts correct.
  // Case-insensitive filename heuristics (Rock_Normal.png, foo_HEIGHT.png).
  char lower[128] = "";
  const char *name = GetFileName(srcPath);
  if (name != NULL) {
    for (int i = 0; name[i] != '\0' && i < (int)sizeof(lower) - 1; i++)
      lower[i] = (char)tolower((unsigned char)name[i]);
  }
  p->isNormalMap = (strstr(lower, "normal") != NULL);
  bool isDisplacement = (strstr(lower, "height") != NULL || strstr(lower, "disp") != NULL);
  snprintf(p->compress, sizeof(p->compress), p->isNormalMap ? "bc3" : (isDisplacement ? "none" : "bc1"));
}

// Load <src>.import; if absent, write it with defaults (Godot-style: the
// sidecar IS the authored record of how this asset imports).
static void ImportParamsLoad(const char *srcPath, BrushTexImportParams *p) {
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
}

static uint32_t Fnv1a(const void *data, int len) {
  const unsigned char *b = data;
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

static uint32_t ImportParamsHash(const BrushTexImportParams *p) {
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

// --- BC1/BC3 encoding (stb_dxt) ------------------------------------------------
// Encode ONE uncompressed RGBA8 mip level into BC blocks. Levels smaller
// than a block (the 2x2/1x1 chain tail) pad by edge-clamping — the chain
// MUST reach 1x1 or trilinear sampling reads an incomplete texture (black):
// raylib never clamps GL_TEXTURE_MAX_LEVEL.
static void EncodeBCLevel(const unsigned char *rgba, int w, int h, bool bc3,
                          bool swizzleNm, unsigned char *out) {
  int blockBytes = bc3 ? 16 : 8;
  unsigned char block[16 * 4];
  for (int by = 0; by < h; by += 4) {
    for (int bx = 0; bx < w; bx += 4) {
      for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
          int sx = bx + col, sy = by + row;
          if (sx > w - 1) sx = w - 1; // edge clamp for partial blocks
          if (sy > h - 1) sy = h - 1;
          memcpy(block + (row * 4 + col) * 4, rgba + ((sy * w + sx) * 4), 4);
        }
      }
      if (swizzleNm) {
        // DXT5nm: X rides the high-quality BC3 alpha channel, Y stays in
        // green (the best-endpoint RGB channel); R/B are don't-cares.
        for (int px = 0; px < 16; px++) {
          unsigned char *p = block + px * 4;
          p[3] = p[0]; // A = X
          p[0] = 255;  // R = 1
          p[2] = 0;    // B = 0 (G already = Y)
        }
      }
      stb_compress_dxt_block(out, block, bc3 ? 1 : 0, STB_DXT_HIGHQUAL);
      out += blockBytes;
    }
  }
}

// A chain is BC-compressible only if EVERY level's byte size agrees with
// raylib's mip walk (w*h*bpp/8, clamped to one block only when BOTH dims
// are < 4). Square power-of-two sources pass down to 1x1; things like
// 512x256 hit a 4x2 level and must cook uncompressed.
static bool ChainCompressible(int w, int h, int mips) {
  for (int m = 0; m < mips; m++) {
    bool aligned = (w % 4 == 0 && h % 4 == 0);
    bool subBlock = (w < 4 && h < 4);
    if (!aligned && !subBlock) return false;
    if (w > 1) w /= 2;
    if (h > 1) h /= 2;
  }
  return true;
}

// Cook one source image into a .ctex. Pure CPU — safe on the worker.
static bool CookTexture(const char *srcPath, const char *dstPath,
                        const BrushTexImportParams *prm) {
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

  // BC compression: transcode the WHOLE uncompressed chain (down to 1x1 —
  // partial chains are GL-incomplete under trilinear and sample black).
  bool wantBC = (strcmp(prm->compress, "bc1") == 0 ||
                 strcmp(prm->compress, "bc3") == 0);
  bool bc3 = (strcmp(prm->compress, "bc3") == 0);
  bool swizzleNm = bc3 && prm->isNormalMap;
  if (wantBC && !ChainCompressible(img.width, img.height, img.mipmaps)) {
    TraceLog(LOG_WARNING,
             "ASSETS: %s (%dx%d) has non-4-aligned mip levels — cooking "
             "uncompressed (use square power-of-two sources for BC)",
             srcPath, img.width, img.height);
    wantBC = false;
  }
  bool appliedSwizzle = false;
  if (wantBC) {
    int bcFormat = bc3 ? PIXELFORMAT_COMPRESSED_DXT5_RGBA
                       : PIXELFORMAT_COMPRESSED_DXT1_RGB;
    int total = MipChainSize(img.width, img.height, bcFormat, img.mipmaps);
    unsigned char *bcData = malloc((size_t)total);
    if (bcData != NULL) {
      const unsigned char *srcMip = img.data;
      unsigned char *dstMip = bcData;
      int w = img.width, hh = img.height;
      for (int m = 0; m < img.mipmaps; m++) {
        EncodeBCLevel(srcMip, w, hh, bc3, swizzleNm, dstMip);
        srcMip += GetPixelDataSize(w, hh, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        dstMip += GetPixelDataSize(w, hh, bcFormat);
        if (w > 1) w /= 2;
        if (hh > 1) hh /= 2;
      }
      free(img.data);
      img.data = bcData;
      img.format = bcFormat;
      // mip count unchanged: full chain, now in BC blocks
      appliedSwizzle = swizzleNm;
    }
  }

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
  h.reserved = appliedSwizzle ? 1u : 0u; // bit0: DXT5nm-swizzled normal map

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
                      const BrushTexImportParams *prm) {
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

// GPU-upload a .ctex blob (header + packed mips). MAIN THREAD only.
// `outSwizzled` (optional) reports the DXT5nm flag.
static Texture2D LoadCtexFromMemory(const unsigned char *blob, int blobSize,
                                    bool *outSwizzled) {
  Texture2D tex = {0};
  if (outSwizzled != NULL) *outSwizzled = false;
  if (blob == NULL || blobSize < (int)sizeof(CtexHeader)) return tex;
  CtexHeader h;
  memcpy(&h, blob, sizeof(h));
  if (h.magic != BRUSH_CTEX_MAGIC) return tex;
  int dataSize = MipChainSize(h.width, h.height, (int)h.format, h.mipCount);
  if (blobSize < (int)sizeof(h) + dataSize) return tex;
  if (outSwizzled != NULL) *outSwizzled = ((h.reserved & 1u) != 0);
  tex.id = rlLoadTexture(blob + sizeof(h), h.width, h.height, (int)h.format,
                         h.mipCount);
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

// File wrapper around LoadCtexFromMemory.
static Texture2D LoadCtex(const char *ctexPath, bool *outSwizzled) {
  if (outSwizzled != NULL) *outSwizzled = false;
  FILE *f = fopen(ctexPath, "rb");
  if (f == NULL) return (Texture2D){0};
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *blob = malloc((size_t)size);
  Texture2D tex = {0};
  if (blob != NULL && fread(blob, (size_t)size, 1, f) == 1)
    tex = LoadCtexFromMemory(blob, (int)size, outSwizzled);
  free(blob);
  fclose(f);
  return tex;
}

static bool IsCookable(const char *path) {
  return IsFileExtension(path, ".png") || IsFileExtension(path, ".jpg") ||
         IsFileExtension(path, ".jpeg") || IsFileExtension(path, ".tga") ||
         IsFileExtension(path, ".bmp");
}

// --- .pak archive (release builds — see docs/asset-pipeline.md §4) ---------------
// Layout: [PakHeader][data blocks, 16-aligned][index]. The index is an
// array sorted by 64-bit FNV-1a path hash (binary search + path confirm;
// equal hashes sit adjacent). Written by tools/packager.

#define BRUSH_PAK_MAGIC 0x314B5042u // 'BPK1'
#define BRUSH_PAK_VERSION 1

typedef struct PakHeader {
  uint32_t magic;
  uint32_t version;
  uint64_t indexOffset;
} PakHeader;

typedef struct PakEntry {
  uint64_t hash;
  uint64_t offset, size;
  uint32_t pathOffset; // into g_pakPaths
} PakEntry;

static FILE *g_pak = NULL;
static PakEntry *g_pakEntries = NULL;
static int g_pakCount = 0;
static char *g_pakPaths = NULL;

static uint64_t Fnv1a64(const char *s) {
  uint64_t h = 14695981039346656037ull;
  while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211ull;
  return h;
}

static const PakEntry *PakFind(const char *path) {
  if (g_pak == NULL || path == NULL) return NULL;
  uint64_t hash = Fnv1a64(path);
  int lo = 0, hi = g_pakCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (g_pakEntries[mid].hash < hash) lo = mid + 1;
    else if (g_pakEntries[mid].hash > hash) hi = mid - 1;
    else {
      // Walk the run of equal hashes and confirm by path.
      int i = mid;
      while (i > 0 && g_pakEntries[i - 1].hash == hash) i--;
      for (; i < g_pakCount && g_pakEntries[i].hash == hash; i++)
        if (strcmp(g_pakPaths + g_pakEntries[i].pathOffset, path) == 0)
          return &g_pakEntries[i];
      return NULL;
    }
  }
  return NULL;
}

// malloc'd copy of an entry's bytes (caller frees). Main thread (one FILE*).
static unsigned char *PakRead(const PakEntry *e, int *outSize) {
  unsigned char *data = malloc((size_t)e->size);
  if (data == NULL) return NULL;
  if (fseeko(g_pak, (off_t)e->offset, SEEK_SET) != 0 ||
      fread(data, (size_t)e->size, 1, g_pak) != 1) {
    free(data);
    return NULL;
  }
  if (outSize != NULL) *outSize = (int)e->size;
  return data;
}

// raylib file-load callbacks: pak first, then plain disk (the default
// behavior we replace). Covers LoadModel/LoadFileText/LoadFileData for
// every project-relative path; absolute engine paths miss the pak and
// fall through to disk.
static unsigned char *PakLoadFileData(const char *fileName, int *dataSize) {
  *dataSize = 0;
  // LOOSE FILES OVERRIDE THE PAK: a stale dev pak must never shadow fresh
  // sources (release folders carry no loose assets, so the pak still
  // serves everything there).
  FILE *f = fopen(fileName, "rb");
  if (f != NULL) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = (size > 0) ? malloc((size_t)size) : NULL;
    if (data != NULL && fread(data, (size_t)size, 1, f) == 1)
      *dataSize = (int)size;
    else { free(data); data = NULL; }
    fclose(f);
    if (data != NULL) return data;
  }
  const PakEntry *e = PakFind(fileName);
  if (e != NULL) return PakRead(e, dataSize);
  TraceLog(LOG_WARNING, "FILEIO: [%s] Failed to open file", fileName);
  return NULL;
}

static char *PakLoadFileText(const char *fileName) {
  int size = 0;
  unsigned char *data = PakLoadFileData(fileName, &size);
  if (data == NULL) return NULL;
  char *text = realloc(data, (size_t)size + 1);
  if (text == NULL) { free(data); return NULL; }
  text[size] = '\0';
  return text;
}

bool BrushAssetsMount(const char *pakPath) {
  FILE *f = fopen(pakPath, "rb");
  if (f == NULL) return false;
  PakHeader h;
  if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != BRUSH_PAK_MAGIC ||
      h.version != BRUSH_PAK_VERSION) {
    TraceLog(LOG_WARNING, "ASSETS: %s is not a valid pak", pakPath);
    fclose(f);
    return false;
  }
  if (fseeko(f, (off_t)h.indexOffset, SEEK_SET) != 0) { fclose(f); return false; }
  uint32_t count = 0, pathBytes = 0;
  if (fread(&count, sizeof(count), 1, f) != 1 ||
      fread(&pathBytes, sizeof(pathBytes), 1, f) != 1) { fclose(f); return false; }
  PakEntry *entries = malloc(sizeof(PakEntry) * count);
  char *paths = malloc(pathBytes);
  bool ok = (entries != NULL && paths != NULL);
  for (uint32_t i = 0; ok && i < count; i++) {
    ok = fread(&entries[i].hash, 8, 1, f) == 1 &&
         fread(&entries[i].offset, 8, 1, f) == 1 &&
         fread(&entries[i].size, 8, 1, f) == 1 &&
         fread(&entries[i].pathOffset, 4, 1, f) == 1;
  }
  if (ok) ok = (fread(paths, pathBytes, 1, f) == 1);
  if (!ok) {
    TraceLog(LOG_WARNING, "ASSETS: %s index unreadable", pakPath);
    free(entries);
    free(paths);
    fclose(f);
    return false;
  }
  g_pak = f;
  g_pakEntries = entries;
  g_pakCount = (int)count;
  g_pakPaths = paths;
  SetLoadFileDataCallback(PakLoadFileData);
  SetLoadFileTextCallback(PakLoadFileText);
  TraceLog(LOG_INFO, "ASSETS: mounted %s (%d entries)", pakPath, g_pakCount);
  return true;
}

// --- Cook worker (background re-imports) -----------------------------------------

typedef enum { JOB_EMPTY = 0, JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAILED } JobState;

typedef struct CookJob {
  char src[BRUSH_ASSETS_PATH_MAX];
  char dst[BRUSH_ASSETS_PATH_MAX + 32];
  BrushTexImportParams params;
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
                      const BrushTexImportParams *prm) {
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
  bool normalSwizzled; // cooked with the DXT5nm profile
  unsigned int prevId; // GL id this entry held before its last re-import
                       // swap — lets holders of the OLD Texture2D value
                       // still release their reference (one swap deep;
                       // callers refresh on every BrushAssetsUpdate)
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

// Disk source first (cook/cache — fresh files always win), then the pak's
// cooked entry (release), then raw LoadTexture as the last resort.
static Texture2D AcquireTexture(TexEntry *e) {
  const char *path = e->path;
  if (g_pak != NULL && !FileExists(path)) {
    char ctex[BRUSH_ASSETS_PATH_MAX + 32];
    CtexPath(path, ctex, sizeof(ctex));
    const PakEntry *pe = PakFind(ctex);
    if (pe != NULL) {
      int size = 0;
      unsigned char *blob = PakRead(pe, &size);
      Texture2D t = LoadCtexFromMemory(blob, size, &e->normalSwizzled);
      free(blob);
      if (t.id != 0) return t; // pak textures are final: no watch/re-import
    }
  }
  if (IsCookable(path) && FileExists(path)) {
    BrushTexImportParams prm;
    ImportParamsLoad(path, &prm);
    char ctex[BRUSH_ASSETS_PATH_MAX + 32];
    CtexPath(path, ctex, sizeof(ctex));
    if (!CtexValid(path, ctex, &prm)) CookTexture(path, ctex, &prm);
    Texture2D t = LoadCtex(ctex, &e->normalSwizzled);
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
    if (g_tex[i].tex.id != tex.id && g_tex[i].prevId != tex.id) continue;
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
        bool swizzled = false;
        Texture2D fresh = LoadCtex(doneDst[d], &swizzled);
        if (fresh.id == 0)
          TraceLog(LOG_WARNING, "ASSETS: re-import upload failed for %s",
                   doneSrc[d]);
        if (fresh.id != 0) {
          e->prevId = e->tex.id;
          if (e->tex.id != 0) UnloadTexture(e->tex);
          e->tex = fresh;
          e->normalSwizzled = swizzled;
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
      BrushTexImportParams prm;
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

void BrushAssetsGetImportParams(const char *path, BrushTexImportParams *out) {
  ImportParamsLoad(path, out);
}

bool BrushAssetsSetImportParams(const char *path,
                                const BrushTexImportParams *p) {
  char imp[BRUSH_ASSETS_PATH_MAX + 16];
  snprintf(imp, sizeof(imp), "%s.import", path);
  FILE *f = fopen(imp, "w");
  if (f == NULL) return false;
  fprintf(f, "[params]\n");
  fprintf(f, "max_size = %d\n", p->maxSize);
  fprintf(f, "generate_mipmaps = %s\n", p->mipmaps ? "true" : "false");
  fprintf(f, "compress = %s\n", p->compress);
  fprintf(f, "is_normal_map = %s\n", p->isNormalMap ? "true" : "false");
  fclose(f);
  return true; // the live watch re-imports from here
}

bool BrushAssetsIsSwizzledNormal(Texture2D tex) {
  if (tex.id == 0) return false;
  for (int i = 0; i < g_texCount; i++)
    if (g_tex[i].tex.id == tex.id) return g_tex[i].normalSwizzled;
  return false;
}

// --- Model registry --------------------------------------------------------------

#define BRUSH_ASSETS_MAX_MODELS 64

typedef struct ModelEntry {
  char path[BRUSH_ASSETS_PATH_MAX];
  Model model; // meshCount 0 = load failed (negative cache)
  int refs;
  BoundingBox aabb; // mesh-space AABB (pre model.transform), for cull bounds
} ModelEntry;

static ModelEntry g_models[BRUSH_ASSETS_MAX_MODELS];
static int g_modelCount = 0;

// raylib 5.5's GenMeshTangents walks vertices as loose triangles (`i += 3`,
// ignoring the index buffer), so it computes garbage on indexed glTF — which
// is nearly every downloaded asset. This generator uses the index buffer
// (Lengyel's method: accumulate a per-vertex tangent frame across the shared
// triangles, then Gram-Schmidt against the normal and store handedness in .w),
// then uploads the tangent VBO into the already-loaded VAO the same way raylib
// does. Correct for indexed AND unindexed (unindexed = trivial index run).
static void GenMeshTangentsIndexed(Mesh *mesh) {
  if (mesh->vertices == NULL || mesh->texcoords == NULL ||
      mesh->normals == NULL || mesh->vertexCount <= 0)
    return;

  if (mesh->tangents == NULL)
    mesh->tangents = (float *)malloc((size_t)mesh->vertexCount * 4 * sizeof(float));
  float *tan1 = (float *)calloc((size_t)mesh->vertexCount * 3, sizeof(float));
  float *tan2 = (float *)calloc((size_t)mesh->vertexCount * 3, sizeof(float));
  if (mesh->tangents == NULL || tan1 == NULL || tan2 == NULL) {
    free(tan1);
    free(tan2);
    return;
  }

  for (int t = 0; t < mesh->triangleCount; t++) {
    int i0, i1, i2;
    if (mesh->indices != NULL) {
      i0 = mesh->indices[t * 3 + 0];
      i1 = mesh->indices[t * 3 + 1];
      i2 = mesh->indices[t * 3 + 2];
    } else {
      i0 = t * 3 + 0;
      i1 = t * 3 + 1;
      i2 = t * 3 + 2;
    }
    const float *p0 = &mesh->vertices[i0 * 3];
    const float *p1 = &mesh->vertices[i1 * 3];
    const float *p2 = &mesh->vertices[i2 * 3];
    const float *w0 = &mesh->texcoords[i0 * 2];
    const float *w1 = &mesh->texcoords[i1 * 2];
    const float *w2 = &mesh->texcoords[i2 * 2];

    float x1 = p1[0] - p0[0], y1 = p1[1] - p0[1], z1 = p1[2] - p0[2];
    float x2 = p2[0] - p0[0], y2 = p2[1] - p0[1], z2 = p2[2] - p0[2];
    float s1 = w1[0] - w0[0], t1 = w1[1] - w0[1];
    float s2 = w2[0] - w0[0], t2 = w2[1] - w0[1];

    float div = s1 * t2 - s2 * t1;
    float r = (fabsf(div) < 0.0001f) ? 0.0f : 1.0f / div;
    float sx = (t2 * x1 - t1 * x2) * r, sy = (t2 * y1 - t1 * y2) * r,
          sz = (t2 * z1 - t1 * z2) * r;
    float tx = (s1 * x2 - s2 * x1) * r, ty = (s1 * y2 - s2 * y1) * r,
          tz = (s1 * z2 - s2 * z1) * r;

    int idx[3] = {i0, i1, i2};
    for (int k = 0; k < 3; k++) {
      float *a = &tan1[idx[k] * 3];
      float *b = &tan2[idx[k] * 3];
      a[0] += sx; a[1] += sy; a[2] += sz;
      b[0] += tx; b[1] += ty; b[2] += tz;
    }
  }

  for (int i = 0; i < mesh->vertexCount; i++) {
    float nx = mesh->normals[i * 3 + 0], ny = mesh->normals[i * 3 + 1],
          nz = mesh->normals[i * 3 + 2];
    float tx = tan1[i * 3 + 0], ty = tan1[i * 3 + 1], tz = tan1[i * 3 + 2];

    // Gram-Schmidt: T' = T - N * dot(N, T)
    float ndt = nx * tx + ny * ty + nz * tz;
    float ox = tx - nx * ndt, oy = ty - ny * ndt, oz = tz - nz * ndt;
    float olen = sqrtf(ox * ox + oy * oy + oz * oz);
    if (olen < 0.0001f) {
      // Degenerate/zero tangent (bad or missing UVs) — synthesise one.
      if (fabsf(nz) > 0.707f) { ox = 1.0f; oy = 0.0f; oz = 0.0f; }
      else {
        ox = -ny; oy = nx; oz = 0.0f;
        float l = sqrtf(ox * ox + oy * oy + oz * oz);
        if (l > 1e-6f) { ox /= l; oy /= l; oz /= l; }
      }
    } else {
      ox /= olen; oy /= olen; oz /= olen;
    }

    // Handedness = sign(dot(cross(N, T'), tan2))
    float cx = ny * oz - nz * oy, cy = nz * ox - nx * oz, cz = nx * oy - ny * ox;
    float bx = tan2[i * 3 + 0], by = tan2[i * 3 + 1], bz = tan2[i * 3 + 2];
    float handed = (cx * bx + cy * by + cz * bz) < 0.0f ? -1.0f : 1.0f;

    mesh->tangents[i * 4 + 0] = ox;
    mesh->tangents[i * 4 + 1] = oy;
    mesh->tangents[i * 4 + 2] = oz;
    mesh->tangents[i * 4 + 3] = handed;
  }

  free(tan1);
  free(tan2);

  // LoadModel already uploaded this mesh, so a mesh that had no tangents has no
  // tangent VBO. Push the CPU tangents into the existing VAO, mirroring what
  // raylib's GenMeshTangents does at its tail — without this the shader reads a
  // zero tangent attribute.
  if (mesh->vboId != NULL) {
    if (mesh->vboId[SHADER_LOC_VERTEX_TANGENT] != 0)
      rlUpdateVertexBuffer(mesh->vboId[SHADER_LOC_VERTEX_TANGENT], mesh->tangents,
                           mesh->vertexCount * 4 * sizeof(float), 0);
    else
      mesh->vboId[SHADER_LOC_VERTEX_TANGENT] = rlLoadVertexBuffer(
          mesh->tangents, mesh->vertexCount * 4 * sizeof(float), false);
    rlEnableVertexArray(mesh->vaoId);
    rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TANGENT, 4, RL_FLOAT, 0, 0, 0);
    rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TANGENT);
    rlDisableVertexArray();
  }
}

Model BrushAssetsModel(const char *path) {
  if (path == NULL || path[0] == '\0') return (Model){0};

  for (int i = 0; i < g_modelCount; i++) {
    if (strcmp(g_models[i].path, path) == 0) {
      g_models[i].refs++;
      return g_models[i].model;
    }
  }

  if (g_modelCount >= BRUSH_ASSETS_MAX_MODELS) {
    TraceLog(LOG_WARNING, "ASSETS: model cache full (%d), can't load %s",
             BRUSH_ASSETS_MAX_MODELS, path);
    return (Model){0};
  }

  ModelEntry *e = &g_models[g_modelCount++];
  memset(e, 0, sizeof(*e));
  strncpy(e->path, path, sizeof(e->path) - 1);
  e->refs = 1;
  e->model = LoadModel(path); // pak-aware via the file-load hooks
  if (e->model.meshCount == 0) {
    TraceLog(LOG_WARNING, "ASSETS: missing/empty model %s", path);
    return e->model;
  }
  // glTF loads prepend raylib's default material at index 0 (real materials
  // shift to 1..N) — bind the lit shader to EVERY material, never by index.
  extern Shader BrushGetLitShader(void);
  for (int i = 0; i < e->model.materialCount; i++)
    e->model.materials[i].shader = BrushGetLitShader();
  // Tangents make normal maps work on the UV path; many exports omit them.
  // Our own generator (unlike raylib 5.5) walks the index buffer, so indexed
  // glTF gets correct tangents instead of geometric-normal fallback.
  for (int i = 0; i < e->model.meshCount; i++)
    if (e->model.meshes[i].tangents == NULL &&
        e->model.meshes[i].texcoords != NULL)
      GenMeshTangentsIndexed(&e->model.meshes[i]);
  // Mesh-space AABB (union over meshes, WITHOUT model.transform) for instance
  // cull bounds — the draw applies the full instance matrix, so bake nothing.
  e->aabb = GetMeshBoundingBox(e->model.meshes[0]);
  for (int i = 1; i < e->model.meshCount; i++) {
    BoundingBox b = GetMeshBoundingBox(e->model.meshes[i]);
    e->aabb.min = Vector3Min(e->aabb.min, b.min);
    e->aabb.max = Vector3Max(e->aabb.max, b.max);
  }
  TraceLog(LOG_INFO, "ASSETS: loaded model %s (%d meshes, %d materials)",
           path, e->model.meshCount, e->model.materialCount);
  return e->model;
}

BoundingBox BrushAssetsModelAABB(const char *path) {
  if (path != NULL)
    for (int i = 0; i < g_modelCount; i++)
      if (strcmp(g_models[i].path, path) == 0) return g_models[i].aabb;
  return (BoundingBox){0};
}

// --- Model collision-shape cache ----------------------------------------------
// One cooked base shape per (path, meshIndex), shared by every instance of the
// model. Cooked in the model's LOCAL space (its base transform baked, instance
// transform not) so callers place it per instance via
// BrushPhysicsAddStaticShapeAt. Model lifetime: freed when the model unloads or
// at shutdown. Bodies hold their own Jolt refs, so removing a body never
// invalidates the cache. A cached NULL means "known uncookable" (won't retry).

#define BRUSH_ASSETS_MAX_SHAPES 256

typedef struct ShapeEntry {
  char path[BRUSH_ASSETS_PATH_MAX];
  int meshIndex;
  JPH_Shape *shape; // NULL = negative-cached
} ShapeEntry;

static ShapeEntry g_shapes[BRUSH_ASSETS_MAX_SHAPES];
static int g_shapeCount = 0;

JPH_Shape *BrushAssetsModelShape(const char *path, int meshIndex) {
  if (path == NULL || path[0] == '\0' || meshIndex < 0) return NULL;

  for (int i = 0; i < g_shapeCount; i++)
    if (g_shapes[i].meshIndex == meshIndex &&
        strcmp(g_shapes[i].path, path) == 0)
      return g_shapes[i].shape; // borrowed (NULL = known uncookable)

  ModelEntry *me = NULL;
  for (int i = 0; i < g_modelCount; i++)
    if (strcmp(g_models[i].path, path) == 0) {
      me = &g_models[i];
      break;
    }
  if (me == NULL || me->model.meshCount == 0) {
    TraceLog(LOG_WARNING, "ASSETS: collision shape requested for non-resident model %s", path);
    return NULL;
  }
  if (meshIndex >= me->model.meshCount) return NULL;
  if (g_shapeCount >= BRUSH_ASSETS_MAX_SHAPES) {
    TraceLog(LOG_WARNING, "ASSETS: shape cache full (%d)", BRUSH_ASSETS_MAX_SHAPES);
    return NULL;
  }

  // Bake the model's own base transform (glTF axis/node conversion) so the
  // shape is shared across instances; the instance transform is applied later
  // by the body/ScaledShape. Runs once per unique mesh (the expensive cook).
  JPH_Shape *shape = BrushPhysicsCookMeshShape(me->model.meshes[meshIndex],
                                               me->model.transform);
  TraceLog(LOG_INFO, "ASSETS: cooked collision shape %s[mesh %d] (%d tris)",
           path, meshIndex, me->model.meshes[meshIndex].triangleCount);
  ShapeEntry *e = &g_shapes[g_shapeCount++];
  memset(e, 0, sizeof(*e));
  strncpy(e->path, path, sizeof(e->path) - 1);
  e->meshIndex = meshIndex;
  e->shape = shape;
  return shape;
}

static void ReleaseModelShapes(const char *path) {
  for (int i = 0; i < g_shapeCount;) {
    if (strcmp(g_shapes[i].path, path) == 0) {
      if (g_shapes[i].shape) BrushPhysicsReleaseShape(g_shapes[i].shape);
      g_shapes[i] = g_shapes[--g_shapeCount]; // swap-remove
    } else {
      i++;
    }
  }
}

void BrushAssetsReleaseModel(const char *path) {
  if (path == NULL || path[0] == '\0') return;
  for (int i = 0; i < g_modelCount; i++) {
    if (strcmp(g_models[i].path, path) != 0) continue;
    if (--g_models[i].refs > 0) return;
    ReleaseModelShapes(g_models[i].path); // free cooked shapes (Jolt still up)
    if (g_models[i].model.meshCount > 0) UnloadModel(g_models[i].model);
    TraceLog(LOG_DEBUG, "ASSETS: unloaded model %s", g_models[i].path);
    g_models[i] = g_models[--g_modelCount]; // swap-remove
    return;
  }
}

int BrushAssetsCookTree(const char *dir) {
  int cooked = 0;
  FilePathList fl = LoadDirectoryFilesEx(dir, NULL, true);
  for (unsigned int i = 0; i < fl.count; i++) {
    const char *path = fl.paths[i];
    if (!IsCookable(path)) continue;
    BrushTexImportParams prm;
    ImportParamsLoad(path, &prm);
    char ctex[BRUSH_ASSETS_PATH_MAX + 32];
    CtexPath(path, ctex, sizeof(ctex));
    if (CtexValid(path, ctex, &prm)) continue;
    if (CookTexture(path, ctex, &prm)) cooked++;
    else TraceLog(LOG_WARNING, "ASSETS: cook failed for %s", path);
  }
  UnloadDirectoryFiles(fl);
  return cooked;
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
  // Free cooked collision shapes first — the caller must run this before
  // BrushPhysicsCleanup (JPH_Shutdown), or destroying a shape here is unsafe.
  for (int i = 0; i < g_shapeCount; i++)
    if (g_shapes[i].shape) BrushPhysicsReleaseShape(g_shapes[i].shape);
  g_shapeCount = 0;
  for (int i = 0; i < g_modelCount; i++)
    if (g_models[i].model.meshCount > 0) UnloadModel(g_models[i].model);
  g_modelCount = 0;
  if (g_pak != NULL) {
    fclose(g_pak);
    g_pak = NULL;
    free(g_pakEntries);
    g_pakEntries = NULL;
    free(g_pakPaths);
    g_pakPaths = NULL;
    g_pakCount = 0;
  }
}
