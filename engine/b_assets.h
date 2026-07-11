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

// Unload everything regardless of refcounts (engine shutdown).
void BrushAssetsShutdown(void);

#ifdef __cplusplus
}
#endif

#endif // B_ASSETS_H
