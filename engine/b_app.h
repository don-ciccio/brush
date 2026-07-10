/*******************************************************************************************
 *   b_app.h - Window, fixed-timestep loop, and game callbacks
 *
 *   The engine owns the main loop. A game provides callbacks:
 *
 *     fixedUpdate  - simulation in fixed 1/60 s steps (deterministic feel at
 *                    any render rate; substeps are capped to avoid the spiral
 *                    of death)
 *     update       - once per rendered frame (input, camera); `alpha` is the
 *                    interpolation factor between the last two fixed steps,
 *                    for smoothing rendered positions
 *     draw         - submit 3D work to the render layers, then call
 *                    BrushRenderExecute()
 *     drawUI       - 2D overlay (HUD, menus), drawn after the 3D frame
 *
 *   The app layer also owns the debug hotkeys (F1 console, F2 layer view) and
 *   the BRUSH_AUTO_SCREENSHOT harness (screenshot at frame 180, then exit)
 *   used for automated visual verification.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_APP_H
#define B_APP_H

#include <raylib.h>
#include <stdbool.h>

#define BRUSH_FIXED_TIMESTEP (1.0f / 60.0f)
#define BRUSH_MAX_SUBSTEPS 5

typedef struct BrushConfig {
  int width;        // window width (0 -> 1600)
  int height;       // window height (0 -> 900)
  const char *title;
  int targetFPS;    // 0 -> 60
  bool noVsync;     // set for uncapped profiling runs
} BrushConfig;

typedef struct BrushCallbacks {
  void (*init)(void *user);
  void (*fixedUpdate)(void *user, float dt); // dt == BRUSH_FIXED_TIMESTEP
  void (*update)(void *user, float dt, float alpha);
  void (*draw)(void *user);
  void (*drawUI)(void *user);
  void (*shutdown)(void *user);
  void *user; // passed to every callback
} BrushCallbacks;

// Run the full app lifecycle: window, engine systems, loop, teardown.
void BrushRun(BrushConfig cfg, BrushCallbacks cb);

// Request a clean exit at the end of the current frame.
void BrushQuit(void);

#endif // B_APP_H
