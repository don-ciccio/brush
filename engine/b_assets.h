/*******************************************************************************************
 *   b_assets.h - Reference-counted asset registry
 *
 *   One cache keyed by path: the first request loads the asset, later
 *   requests share it, releases drop the refcount, and the last release
 *   unloads the GPU object. This is the layer that lets five blocks share
 *   one stone texture, lets scene hot-reload swap content centrally, and
 *   (next) will cache models the same way.
 *
 *   v1 caches TEXTURES. Loads get the full quality treatment once —
 *   mipmaps + trilinear filtering + repeat wrap — so call sites never
 *   re-derive it. A missing file returns a zero-id texture and warns once
 *   per path (negative entries are cached too, so a hot-reload loop doesn't
 *   spam the log).
 *
 *   IMPORT CACHE (docs/asset-pipeline.md, Phase 1): source images cook
 *   into .brush/imported/<path>.ctex — decoded, downscaled to the sidecar's
 *   max_size, full CPU mip chain — so warm loads skip PNG inflate and GPU
 *   mip generation entirely. Each source gets a <path>.import sidecar with
 *   its parameters (created with defaults on first import). The .ctex
 *   header stores source size+mtime, a params hash, and the importer
 *   version; any mismatch re-imports silently. Cook failure falls back to
 *   the raw load, so nothing ever blocks on the cache.
 *
 *   Threading: the public API is main-thread only (GL objects); RE-imports
 *   of changed sources cook on a background worker and BrushAssetsUpdate
 *   swaps the finished texture in on the main thread.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_ASSETS_H
#define B_ASSETS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <raylib.h>

// Resolve a path relative to the ENGINE installation (the tree holding
// engine/shaders, engine/resources, build/). Found once by walking up from
// the executable's directory; falls back to the cwd if nothing matches, so
// running from the repo root keeps working. Engine-internal loads (shaders,
// lookup textures) MUST use this — the process cwd belongs to the PROJECT
// (the editor/player chdir into the open project; see docs/asset-pipeline.md).
//
// Returns a pointer into a small ring of static buffers: valid until three
// more calls, so `LoadShader(BrushEnginePath(a), BrushEnginePath(b))` is fine.
const char *BrushEnginePath(const char *relative);

// Acquire the texture at `path` (+1 ref). Returns id 0 if the file is
// missing/unreadable.
Texture2D BrushAssetsTexture(const char *path);

// Release one reference to a texture obtained from BrushAssetsTexture.
// Unknown/zero-id textures are ignored, so callers can release
// unconditionally.
void BrushAssetsReleaseTexture(Texture2D tex);

// Per frame: watch loaded sources for edits (30-frame stat cadence), hand
// changed ones to the cook worker, and swap finished re-imports in on this
// (main) thread. Returns true when any texture was replaced — holders of
// Texture2D VALUES must refresh (e.g. BrushSceneResolveMaterials); the
// registry itself is already up to date.
bool BrushAssetsUpdate(void);

// --- Import settings (.import sidecars) --------------------------------------
// The authored parameters for one source texture. `compress`: "none",
// "bc1" (opaque albedo, 8:1) or "bc3" (alpha / normal maps, 4:1) — BC data
// stays compressed in VRAM. Normal maps cooked with bc3 use the DXT5nm
// swizzle (X in alpha, Y in green); the lit shader reconstructs Z. BC
// requires every emitted mip to be a multiple of 4 px — sources that
// can't satisfy it at the base level cook uncompressed with a warning.
typedef struct BrushTexImportParams {
  int maxSize;      // clamp the longest side (0 = no clamp)
  bool mipmaps;     // build the full mip chain offline
  bool isNormalMap; // DXT5nm profile when compressed with bc3
  char compress[8]; // "none" | "bc1" | "bc3"
} BrushTexImportParams;

// Read <path>.import (writing it with defaults first if absent).
void BrushAssetsGetImportParams(const char *path, BrushTexImportParams *out);

// Write <path>.import. The live watch (BrushAssetsUpdate) sees the sidecar
// change and re-imports automatically — this is the whole "apply" story.
bool BrushAssetsSetImportParams(const char *path,
                                const BrushTexImportParams *p);

// True if this registry texture was cooked as a DXT5nm-swizzled normal map
// (consumers must tell the lit shader to reconstruct Z from alpha/green).
bool BrushAssetsIsSwizzledNormal(Texture2D tex);

// --- Models -------------------------------------------------------------------
// Ref-counted like textures, keyed by project-relative path. The registry
// binds the engine lit shader to every material on load (raylib prepends
// its default material at index 0 on glTF — bind ALL, never by index) and
// hands out the SAME Model to every caller: static props only. Anything
// with per-instance animation state (the player character) must load its
// own copy outside the registry. Ship .glb — self-contained, and the only
// form a release pak can serve (.gltf external buffers bypass the pak's
// file hooks).

// Acquire the model at `path` (+1 ref). meshCount == 0 = load failed
// (warned once, negative-cached).
Model BrushAssetsModel(const char *path);

// Release one reference; the last release unloads GPU/CPU data.
void BrushAssetsReleaseModel(const char *path);

// --- Release packaging (.pak) --------------------------------------------------
// Mount a pak archive (tools/packager output) as the top of the lookup
// chain. The pak holds the project tree verbatim — logical asset paths
// (scenes, models, terrain) plus the cooked .brush/imported/ entries — and
// mounting hooks raylib's file-load callbacks, so EVERY engine/raylib read
// of a project-relative path (LoadModel, scene text, sculpt blobs, cooked
// textures) transparently resolves from the archive; paths not in the pak
// fall through to disk. Call once at startup (the player mounts ./game.pak
// when present). Returns false if missing/corrupt.
bool BrushAssetsMount(const char *pakPath);

// Cook every cookable texture under `dir` (recursive) so the import cache
// is complete and current. Pure CPU — the packager runs this headless
// before packing. Returns the number of textures (re)cooked.
int BrushAssetsCookTree(const char *dir);

// Unload everything regardless of refcounts (engine shutdown).
void BrushAssetsShutdown(void);

#ifdef __cplusplus
}
#endif

#endif // B_ASSETS_H
