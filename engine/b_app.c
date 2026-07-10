/*******************************************************************************************
 *   b_app.c - Window, fixed-timestep loop, and game callbacks
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_app.h"
#include "b_console.h"
#include "b_render.h"

#include <stdlib.h>

static bool g_running = false;

void BrushQuit(void) { g_running = false; }

void BrushRun(BrushConfig cfg, BrushCallbacks cb) {
  if (cfg.width <= 0) cfg.width = 1600;
  if (cfg.height <= 0) cfg.height = 900;
  if (cfg.targetFPS <= 0) cfg.targetFPS = 60;
  if (cfg.title == NULL) cfg.title = "brush";

  // Console hooks TraceLog, so it comes up before the window: every init
  // message (GL version, texture loads) lands in the in-game console too.
  BrushConsoleInit();

  bool perfMode = (getenv("BRUSH_PERF") != NULL) || cfg.noVsync;
  if (!perfMode) SetConfigFlags(FLAG_VSYNC_HINT);
  SetConfigFlags(FLAG_MSAA_4X_HINT);

  InitWindow(cfg.width, cfg.height, cfg.title);
  SetTargetFPS(perfMode ? 0 : cfg.targetFPS);

  // ESC belongs to game menus, not the window-close binding.
  SetExitKey(KEY_NULL);

  BrushRenderInit();

  if (cb.init) cb.init(cb.user);

  g_running = true;
  float accumulator = 0.0f;
  int frameCount = 0;

  while (g_running && !WindowShouldClose()) {
    float frameDt = GetFrameTime();

    // Debug hotkeys owned by the engine.
    if (IsKeyPressed(KEY_F1)) BrushConsoleToggle();
    if (IsKeyPressed(KEY_F2)) BrushRenderCycleLayerView();

    // Fixed-timestep simulation with a substep cap: movement feel is
    // identical at any render rate, and a long hitch can't spiral.
    accumulator += frameDt;
    if (accumulator > BRUSH_FIXED_TIMESTEP * BRUSH_MAX_SUBSTEPS)
      accumulator = BRUSH_FIXED_TIMESTEP * BRUSH_MAX_SUBSTEPS;
    while (accumulator >= BRUSH_FIXED_TIMESTEP) {
      if (cb.fixedUpdate) cb.fixedUpdate(cb.user, BRUSH_FIXED_TIMESTEP);
      accumulator -= BRUSH_FIXED_TIMESTEP;
    }
    float alpha = accumulator / BRUSH_FIXED_TIMESTEP;

    if (cb.update) cb.update(cb.user, frameDt, alpha);

    BeginDrawing();
    ClearBackground(BLACK);
    if (cb.draw) cb.draw(cb.user); // game submits layers + BrushRenderExecute
    if (cb.drawUI) cb.drawUI(cb.user);
    BrushConsoleDraw(); // console overlays everything
    EndDrawing();

    if (perfMode) {
      static int perfFrame = 0;
      if (++perfFrame % 20 == 0)
        TraceLog(LOG_INFO, "PERF avg=%.2fms fps=%d", frameDt * 1000.0f,
                 GetFPS());
    }

    // Auto-screenshot harness for automated visual verification.
    frameCount++;
    if (getenv("BRUSH_AUTO_SCREENSHOT") != NULL && frameCount == 180) {
      TakeScreenshot("screenshot.png");
      TraceLog(LOG_INFO, "Auto-screenshot saved to screenshot.png");
      g_running = false;
    }
  }

  if (cb.shutdown) cb.shutdown(cb.user);
  BrushRenderShutdown();
  CloseWindow();
}
