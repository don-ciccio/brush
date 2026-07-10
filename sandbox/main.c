/*******************************************************************************************
 *   sandbox/main.c - brush starter scene
 *
 *   The "new game" template: a capsule mannequin on an endless checkerboard
 *   under the procedural sky. Everything on screen is generated at runtime —
 *   the repo ships zero binary assets.
 *
 *   Demonstrates the intended engine usage:
 *     - BrushRun owns the loop; the game is four callbacks
 *     - simulation in fixedUpdate (1/60 s), rendering from interpolated state
 *     - draws submitted to explicit render layers (opaque / transparent)
 *     - camera, input, console come from the engine
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "brush.h"

#include <math.h>
#include <raymath.h>

// --- Character tuning (units: metres, seconds) ---
#define WALK_SPEED 3.0f
#define RUN_SPEED 6.5f
#define ACCEL 24.0f
#define GRAVITY 20.0f
#define JUMP_VELOCITY 7.5f
#define TURN_RATE 12.0f // yaw smoothing toward the move direction (1/s)

typedef struct Sandbox {
  // Simulation state (fixed steps)
  Vector3 pos, prevPos;
  Vector3 vel;
  float yaw, prevYaw; // radians
  bool grounded;
  bool jumpQueued;

  // Render state (interpolated)
  Vector3 renderPos;
  float renderYaw;

  BrushOrbitCam camera;

  Model floor;
  Model body;   // capsule torso + head baked into one mesh transform set
  Model head;
  Model nose;   // small facing marker
  Model shadowBlob;

  Texture2D checkerTex;
  Texture2D blobTex;

  bool menuOpen;
  int menuSel;
} Sandbox;

// ------------------------------------------------------------------
// Init: build every asset procedurally
// ------------------------------------------------------------------

static void SandboxInit(void *user) {
  Sandbox *s = user;

  // Checkerboard floor, ~1.9 m squares over a 120 m slab.
  Image checker = GenImageChecked(1024, 1024, 16, 16, RAYWHITE,
                                  (Color){196, 199, 206, 255});
  s->checkerTex = LoadTextureFromImage(checker);
  UnloadImage(checker);
  GenTextureMipmaps(&s->checkerTex);
  SetTextureFilter(s->checkerTex, TEXTURE_FILTER_TRILINEAR);

  s->floor = LoadModelFromMesh(GenMeshPlane(120.0f, 120.0f, 1, 1));
  s->floor.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = s->checkerTex;

  // Mannequin: capsule torso + sphere head + a nose cube marking the facing.
  s->body = LoadModelFromMesh(GenMeshCylinder(0.30f, 0.85f, 16));
  s->head = LoadModelFromMesh(GenMeshSphere(0.22f, 12, 16));
  s->nose = LoadModelFromMesh(GenMeshCube(0.10f, 0.10f, 0.22f));

  // Soft radial blob shadow (transparent layer content).
  Image blob = GenImageGradientRadial(256, 256, 0.0f, (Color){0, 0, 0, 150},
                                      (Color){0, 0, 0, 0});
  s->blobTex = LoadTextureFromImage(blob);
  UnloadImage(blob);
  s->shadowBlob = LoadModelFromMesh(GenMeshPlane(1.0f, 1.0f, 1, 1));
  s->shadowBlob.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = s->blobTex;

  // Everything lit by the engine's layered forward shader.
  Shader lit = BrushGetLitShader();
  s->floor.materials[0].shader = lit;
  s->body.materials[0].shader = lit;
  s->head.materials[0].shader = lit;
  s->nose.materials[0].shader = lit;

  s->pos = s->prevPos = s->renderPos = (Vector3){0.0f, 0.0f, 0.0f};
  s->grounded = true;

  BrushOrbitCamInit(&s->camera, s->pos);

  TraceLog(LOG_INFO, "SANDBOX: ready — move with WASD, jump with Space");
}

// ------------------------------------------------------------------
// Simulation (fixed 1/60 s steps)
// ------------------------------------------------------------------

static void SandboxFixedUpdate(void *user, float dt) {
  Sandbox *s = user;
  if (s->menuOpen) return; // menu freezes the sim

  s->prevPos = s->pos;
  s->prevYaw = s->yaw;

  // Camera-relative move direction from the input actions.
  float inX = BrushInputAxis(BRUSH_AXIS_MOVE_X);
  float inY = BrushInputAxis(BRUSH_AXIS_MOVE_Y);

  Vector3 camFwd = Vector3Subtract(s->camera.cam.target, s->camera.cam.position);
  camFwd.y = 0;
  camFwd = Vector3Normalize(camFwd);
  Vector3 camRight = {-camFwd.z, 0, camFwd.x};

  Vector3 wishDir = Vector3Add(Vector3Scale(camFwd, inY),
                               Vector3Scale(camRight, inX));
  float wishLen = Vector3Length(wishDir);
  if (wishLen > 1.0f) wishDir = Vector3Scale(wishDir, 1.0f / wishLen);

  float maxSpeed = BrushInputDown(BRUSH_BTN_SPRINT) ? RUN_SPEED : WALK_SPEED;
  Vector3 wishVel = Vector3Scale(wishDir, maxSpeed * fminf(wishLen, 1.0f));

  // Horizontal acceleration toward the wish velocity.
  float blend = fminf(ACCEL * dt, 1.0f);
  s->vel.x += (wishVel.x - s->vel.x) * blend;
  s->vel.z += (wishVel.z - s->vel.z) * blend;

  // Jump + gravity against the flat ground plane (y = 0).
  if (s->jumpQueued && s->grounded) {
    s->vel.y = JUMP_VELOCITY;
    s->grounded = false;
  }
  s->jumpQueued = false;
  if (!s->grounded) s->vel.y -= GRAVITY * dt;

  s->pos = Vector3Add(s->pos, Vector3Scale(s->vel, dt));
  if (s->pos.y <= 0.0f) {
    s->pos.y = 0.0f;
    s->vel.y = 0.0f;
    s->grounded = true;
  }

  // Face the movement direction (shortest arc, smoothed).
  float horizSpeed = sqrtf(s->vel.x * s->vel.x + s->vel.z * s->vel.z);
  if (horizSpeed > 0.3f) {
    float targetYaw = atan2f(s->vel.x, s->vel.z);
    float diff = targetYaw - s->yaw;
    while (diff > PI) diff -= 2.0f * PI;
    while (diff < -PI) diff += 2.0f * PI;
    s->yaw += diff * fminf(TURN_RATE * dt, 1.0f);
  }
}

// ------------------------------------------------------------------
// Per-frame update (input latching, interpolation, camera, menu)
// ------------------------------------------------------------------

static void SandboxUpdate(void *user, float dt, float alpha) {
  Sandbox *s = user;

  if (BrushInputPressed(BRUSH_BTN_MENU)) {
    s->menuOpen = !s->menuOpen;
    s->menuSel = 0;
  }

  if (s->menuOpen) {
    if (BrushInputPressed(BRUSH_BTN_UP)) s->menuSel = (s->menuSel + 1) % 2;
    if (BrushInputPressed(BRUSH_BTN_DOWN)) s->menuSel = (s->menuSel + 1) % 2;
    if (BrushInputPressed(BRUSH_BTN_ACCEPT)) {
      if (s->menuSel == 0) s->menuOpen = false;
      else BrushQuit();
    }
    return; // no gameplay input while the menu is up
  }

  // Latch jump between fixed steps so a tap on a fast frame isn't lost.
  if (BrushInputPressed(BRUSH_BTN_JUMP)) s->jumpQueued = true;

  // Interpolate sim state for rendering (fixed-step stutter stays invisible).
  s->renderPos = Vector3Lerp(s->prevPos, s->pos, alpha);
  float yawDiff = s->yaw - s->prevYaw;
  while (yawDiff > PI) yawDiff -= 2.0f * PI;
  while (yawDiff < -PI) yawDiff += 2.0f * PI;
  s->renderYaw = s->prevYaw + yawDiff * alpha;

  BrushOrbitCamUpdate(&s->camera, s->renderPos, s->vel, dt);
}

// ------------------------------------------------------------------
// Draw: submit to layers, then execute the stack
// ------------------------------------------------------------------

static void SandboxDraw(void *user) {
  Sandbox *s = user;
  Vector3 p = s->renderPos;
  Matrix rot = MatrixRotateY(s->renderYaw);

  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->floor, MatrixIdentity(), WHITE);

  // Mannequin pieces (torso base at feet, head on top, nose forward).
  Matrix torso = MatrixMultiply(rot, MatrixTranslate(p.x, p.y + 0.05f, p.z));
  Matrix head = MatrixMultiply(rot, MatrixTranslate(p.x, p.y + 1.14f, p.z));
  Matrix nose = MatrixMultiply(
      MatrixMultiply(MatrixTranslate(0, 1.14f, 0.26f), rot),
      MatrixTranslate(p.x, p.y, p.z));
  Color skin = (Color){215, 215, 220, 255};
  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->body, torso, skin);
  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->head, head, skin);
  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->nose, nose,
                    (Color){90, 120, 200, 255});

  // Blob shadow: shrinks and fades with jump height (transparent layer).
  float h = fminf(p.y / 3.0f, 1.0f);
  float shScale = 0.9f * (1.0f - 0.4f * h);
  unsigned char shAlpha = (unsigned char)(255.0f * (1.0f - 0.6f * h));
  Matrix shadow = MatrixMultiply(MatrixScale(shScale, 1.0f, shScale),
                                 MatrixTranslate(p.x, 0.02f, p.z));
  BrushRenderSubmit(BRUSH_LAYER_TRANSPARENT, &s->shadowBlob, shadow,
                    (Color){255, 255, 255, shAlpha});

  BrushRenderExecute(s->camera.cam);
}

// ------------------------------------------------------------------
// UI overlay
// ------------------------------------------------------------------

static void SandboxDrawUI(void *user) {
  Sandbox *s = user;

  DrawText("Press [TAB] to toggle menu", 16, 14, 20, DARKGRAY);
  DrawText("Use keys [W][A][S][D] or GamePad to move character", 16, 40, 20,
           DARKGRAY);
  DrawText("Press [F1] to toggle console", 16, 66, 20, DARKGRAY);
  DrawText(TextFormat("Press [F2] to cycle layer view: %s",
                      BrushRenderLayerViewName()),
           16, 92, 20, DARKGRAY);
  DrawText(TextFormat("%d FPS", GetFPS()), GetScreenWidth() - 110, 14, 24,
           (Color){90, 120, 200, 255});

  if (s->menuOpen) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    DrawRectangle(0, 0, w, h, (Color){0, 0, 0, 120});
    int bx = w / 2 - 160, by = h / 2 - 90;
    DrawRectangle(bx, by, 320, 180, (Color){24, 26, 34, 240});
    DrawRectangleLines(bx, by, 320, 180, (Color){90, 120, 200, 255});
    DrawText("brush sandbox", bx + 24, by + 20, 26, RAYWHITE);
    const char *items[2] = {"Resume", "Quit"};
    for (int i = 0; i < 2; i++) {
      bool sel = (s->menuSel == i);
      DrawText(TextFormat("%s %s", sel ? ">" : " ", items[i]), bx + 24,
               by + 76 + i * 36, 22,
               sel ? (Color){120, 190, 255, 255} : LIGHTGRAY);
    }
  }
}

static void SandboxShutdown(void *user) {
  Sandbox *s = user;
  UnloadModel(s->floor);
  UnloadModel(s->body);
  UnloadModel(s->head);
  UnloadModel(s->nose);
  UnloadModel(s->shadowBlob);
  UnloadTexture(s->checkerTex);
  UnloadTexture(s->blobTex);
}

int main(void) {
  static Sandbox s = {0};
  BrushRun(
      (BrushConfig){.width = 1600, .height = 900, .title = "brush sandbox"},
      (BrushCallbacks){
          .init = SandboxInit,
          .fixedUpdate = SandboxFixedUpdate,
          .update = SandboxUpdate,
          .draw = SandboxDraw,
          .drawUI = SandboxDrawUI,
          .shutdown = SandboxShutdown,
          .user = &s,
      });
  return 0;
}
