/*******************************************************************************************
 *   b_app.c - Window, fixed-timestep loop, and game callbacks
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_app.h"
#include "b_console.h"
#include "b_post.h"
#include "b_render.h"

#include <stdlib.h>
#include <string.h>

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
  // NO MSAA hint: the scene renders into the engine's own HDR FBO (SMAA is
  // the AA); a multisampled backbuffer would only pay memory + a resolve
  // every frame for the HUD text.

  InitWindow(cfg.width, cfg.height, cfg.title);
  SetTargetFPS(perfMode ? 0 : cfg.targetFPS);

  const char *scaleEnv = getenv("BRUSH_RENDER_SCALE");
  if (scaleEnv != NULL) cfg.renderScale = (float)atof(scaleEnv);

  // ESC belongs to game menus, not the window-close binding.
  SetExitKey(KEY_NULL);

  BrushRenderInit(cfg.width, cfg.height, cfg.renderScale);

  if (cb.init) cb.init(cb.user);

  // Harness: BRUSH_VIEW=<name> starts in a debug layer view (matches the F2
  // cycle names, e.g. "normals") so headless captures can isolate a term.
  const char *viewEnv = getenv("BRUSH_VIEW");
  if (viewEnv != NULL)
    for (int i = 0; i < BRUSH_VIEW_COUNT; i++) {
      if (strcmp(BrushRenderLayerViewName(), viewEnv) == 0) break;
      BrushRenderCycleLayerView();
    }

  g_running = true;
  float accumulator = 0.0f;
  int frameCount = 0;

  while (g_running && !WindowShouldClose()) {
    float frameDt = GetFrameTime();

    // Debug hotkeys owned by the engine.
    if (IsKeyPressed(KEY_F1)) BrushConsoleToggle();
    if (IsKeyPressed(KEY_F2)) BrushRenderCycleLayerView();
    if (IsKeyPressed(KEY_F3)) BrushRenderTogglePost();
    if (IsKeyPressed(KEY_F4)) BrushRenderToggleShadows();
    if (IsKeyPressed(KEY_F5)) {
      struct BrushPost *post = BrushRenderGetPost();
      if (post) {
        post->ssaoEnabled = !post->ssaoEnabled;
        TraceLog(LOG_INFO, "BRUSH: SSAO %s", post->ssaoEnabled ? "ON" : "OFF");
      }
    }
    if (IsKeyPressed(KEY_F6)) {
      struct BrushPost *post = BrushRenderGetPost();
      if (post) {
        post->smaaEnabled = !post->smaaEnabled;
        TraceLog(LOG_INFO, "BRUSH: SMAA %s", post->smaaEnabled ? "ON" : "OFF");
      }
    }
    if (IsKeyPressed(KEY_F7)) {
      struct BrushPost *post = BrushRenderGetPost();
      if (post) {
        post->dofEnabled = !post->dofEnabled;
        TraceLog(LOG_INFO, "BRUSH: DOF %s", post->dofEnabled ? "ON" : "OFF");
      }
    }
    if (IsKeyPressed(KEY_F8)) {
      struct BrushPost *post = BrushRenderGetPost();
      if (post) {
        post->godRaysEnabled = !post->godRaysEnabled;
        TraceLog(LOG_INFO, "BRUSH: god rays %s",
                 post->godRaysEnabled ? "ON" : "OFF");
      }
    }
    if (IsKeyPressed(KEY_F9)) {
      struct BrushPost *post = BrushRenderGetPost();
      if (post) {
        post->volFogEnabled = !post->volFogEnabled;
        TraceLog(LOG_INFO, "BRUSH: volumetric fog %s",
                 post->volFogEnabled ? "ON" : "OFF");
      }
    }

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
    // BRUSH_AUTO_FRAMES overrides when the capture fires (default 3 s).
    static int shotFrame = 0;
    if (shotFrame == 0) {
      const char *sf = getenv("BRUSH_AUTO_FRAMES");
      shotFrame = (sf != NULL) ? atoi(sf) : 180;
      if (shotFrame <= 0) shotFrame = 180;
    }
    frameCount++;
    if (getenv("BRUSH_AUTO_SCREENSHOT") != NULL && frameCount == shotFrame) {
      TakeScreenshot("screenshot.png");
      TraceLog(LOG_INFO, "Auto-screenshot saved to screenshot.png");
      g_running = false;
    }
  }

  if (cb.shutdown) cb.shutdown(cb.user);
  BrushRenderShutdown();
  CloseWindow();
}
