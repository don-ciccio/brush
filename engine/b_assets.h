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
 *   Threading: main thread only (GL objects).
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_ASSETS_H
#define B_ASSETS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <raylib.h>

// Acquire the texture at `path` (+1 ref). Returns id 0 if the file is
// missing/unreadable.
Texture2D BrushAssetsTexture(const char *path);

// Release one reference to a texture obtained from BrushAssetsTexture.
// Unknown/zero-id textures are ignored, so callers can release
// unconditionally.
void BrushAssetsReleaseTexture(Texture2D tex);

// Unload everything regardless of refcounts (engine shutdown).
void BrushAssetsShutdown(void);

#ifdef __cplusplus
}
#endif

#endif // B_ASSETS_H
