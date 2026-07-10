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
#include <stdio.h>
#include <stdlib.h>

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
  BrushTimeOfDay tod;

  BrushPhysics phys;
  BrushCharacter body; // Jolt kinematic capsule (walls, steps, slopes)
  float velY;          // vertical velocity (gravity + jumps)
  float airTime;       // continuous seconds off the ground (debounced flag)
  float ikWeight;      // shared slope-IK ramp: 0 airborne -> 1 grounded
  bool crouched;
  float rollTimer;     // seconds of roll movement burst remaining

  BrushWorld *world; // chunk-streamed terrain (flat by default)
  Texture2D groundTex;
  Model crate;
  Model ramp;
  Matrix rampXform;
  Vector3 crateA, crateB, crateB2, crateC; // seated on the terrain
  Vector3 rampPos;

  Model mannequin; // Quaternius UAL mannequin (CC0), skinned + animated
  BrushAnimator animator;

  bool menuOpen;
  int menuSel;
} Sandbox;

// --- Terrain surface: seeded value-noise fbm, rolling hills. Deterministic
// and world-continuous (neighbouring chunks agree at shared edges), reentrant
// (runs on the world's worker thread). This is the ONE function a game writes
// to shape its world; everything else streams from it.
static float HillHash(int x, int z) {
  unsigned int h = (unsigned int)x * 374761393u + (unsigned int)z * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (float)(h & 0xFFFFFF) / (float)0xFFFFFF; // [0,1]
}
static float HillNoise(float x, float z) {
  int xi = (int)floorf(x), zi = (int)floorf(z);
  float fx = x - (float)xi, fz = z - (float)zi;
  float u = fx * fx * (3.0f - 2.0f * fx);
  float v = fz * fz * (3.0f - 2.0f * fz);
  float a = HillHash(xi, zi), b = HillHash(xi + 1, zi);
  float c = HillHash(xi, zi + 1), d = HillHash(xi + 1, zi + 1);
  return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}
static float SandboxHeight(void *user, float wx, float wz) {
  (void)user;
  float freq = 1.0f / 55.0f, amp = 1.0f, sum = 0.0f, norm = 0.0f;
  for (int o = 0; o < 4; o++) {
    sum += (HillNoise(wx * freq, wz * freq) - 0.5f) * amp;
    norm += amp;
    freq *= 2.0f;
    amp *= 0.5f;
  }
  return (sum / norm) * 9.0f; // ~+/- 4.5 m rolling hills
}

// Camera ground clamp: keep the boom above the streamed terrain.
static float CamGround(float x, float z, void *user) {
  return BrushWorldGroundHeight(((Sandbox *)user)->world, x, z);
}

// Foot-IK ground query: height (and normal) under a world position.
static bool GroundUnder(void *user, Vector3 probe, float *outHeight,
                        Vector3 *outNormal) {
  Sandbox *s = user;
  Vector3 start = {probe.x, probe.y + 1.0f, probe.z};
  Vector3 hit, n;
  if (!BrushPhysicsRaycast(&s->phys, start, (Vector3){0, -1, 0}, 3.0f, &hit,
                           &n))
    return false;
  *outHeight = hit.y;
  if (outNormal != NULL) *outNormal = n;
  if (getenv("BRUSH_IK_DBG")) {
    static int dbg = 0;
    if (++dbg % 60 == 0)
      TraceLog(LOG_INFO, "IKDBG probe(%.2f,%.2f,%.2f) hitY=%.2f n=(%.2f,%.2f,%.2f)",
               probe.x, probe.y, probe.z, hit.y, n.x, n.y, n.z);
  }
  return true;
}

// Camera anti-clip: raycast through the physics world so crates/ramps never
// cut between the camera and the character.
static bool CameraObstructed(Vector3 from, Vector3 to, Vector3 *hitPoint,
                             void *user) {
  Sandbox *s = user;
  Vector3 d = Vector3Subtract(to, from);
  float len = Vector3Length(d);
  if (len < 0.001f) return false;
  return BrushPhysicsRaycast(&s->phys, from, Vector3Scale(d, 1.0f / len), len,
                             hitPoint, NULL);
}

// ------------------------------------------------------------------
// Init: build every asset procedurally
// ------------------------------------------------------------------

static void SandboxInit(void *user) {
  Sandbox *s = user;

  // Animated mannequin (CC0). Clips ride in the same GLB; the animator binds
  // them by name (UAL defaults).
  s->mannequin = LoadModel("assets/character/mannequin.glb");
  BrushAnimatorInit(&s->animator, &s->mannequin,
                    "assets/character/mannequin.glb", NULL);
  BoundingBox bb = GetModelBoundingBox(s->mannequin);
  TraceLog(LOG_INFO, "SANDBOX: mannequin bounds y [%.2f .. %.2f], %d bones",
           bb.min.y, bb.max.y, s->mannequin.boneCount);

  // Everything lit by the engine's layered forward shader.
  Shader lit = BrushGetLitShader();
  // glTF loads prepend raylib's default material at index 0 (real materials
  // shift to 1..N) — assign the shader to every material, never by index.
  for (int i = 0; i < s->mannequin.materialCount; i++)
    s->mannequin.materials[i].shader = lit;

  BrushPhysicsInit(&s->phys);

  // Spawn location (BRUSH_SPAWN="x,y,z" overrides for captures).
  float spx = 0.0f, spz = 0.0f;
  const char *spawn = getenv("BRUSH_SPAWN");
  if (spawn != NULL) {
    float sy;
    sscanf(spawn, "%f,%f,%f", &spx, &sy, &spz);
  }

  // Classic checkerboard ground texture (tiles across chunks in world space).
  Image checker = GenImageChecked(1024, 1024, 32, 32, RAYWHITE,
                                  (Color){196, 199, 206, 255});
  s->groundTex = LoadTextureFromImage(checker);
  UnloadImage(checker);
  GenTextureMipmaps(&s->groundTex);
  SetTextureFilter(s->groundTex, TEXTURE_FILTER_TRILINEAR);
  SetTextureWrap(s->groundTex, TEXTURE_WRAP_REPEAT);

  // --- Chunk-streamed open world: FLAT terrain by default (heightFn NULL);
  // BRUSH_HILLS plugs in SandboxHeight to shape rolling hills. Per-chunk Jolt
  // colliders; the initial ring blocks until resident so ground + collision
  // are ready below. Created centered on the spawn. ---
  float (*heightFn)(void *, float, float) =
      (getenv("BRUSH_HILLS") != NULL) ? SandboxHeight : NULL;
  s->world = BrushWorldCreate(
      (BrushWorldConfig){.seed = 1337,
                         .heightFn = heightFn,
                         .chunkSize = 64.0f,
                         .loadRadius = 4,
                         .physics = &s->phys,
                         .groundTex = s->groundTex,
                         .texMetresPerTile = 64.0f}, // 32 squares/chunk -> 2 m
      (Vector3){spx, 0, spz});

  float groundY = BrushWorldGroundHeight(s->world, spx, spz);
  s->pos = s->prevPos = s->renderPos = (Vector3){spx, groundY + 0.2f, spz};
  s->grounded = true;

  // Obstacle course, each piece seated on the terrain surface.
  s->crate = LoadModelFromMesh(GenMeshCube(1.5f, 1.5f, 1.5f));
  s->crate.materials[0].shader = BrushGetLitShader();
  float ga = BrushWorldGroundHeight(s->world, 3.0f, -6.0f);
  float gb = BrushWorldGroundHeight(s->world, -2.5f, -9.0f);
  float gc = BrushWorldGroundHeight(s->world, 0.0f, -10.0f);
  s->crateA = (Vector3){3.0f, ga + 0.75f, -6.0f};
  s->crateB = (Vector3){-2.5f, gb + 0.75f, -9.0f};
  s->crateB2 = (Vector3){-2.5f, gb + 2.25f, -9.0f};
  s->crateC = (Vector3){0.0f, gc + 0.75f, -10.0f};
  BrushPhysicsAddStaticBox(&s->phys, s->crateA, (Vector3){1.5f, 1.5f, 1.5f}, 0, "crate A");
  BrushPhysicsAddStaticBox(&s->phys, s->crateB, (Vector3){1.5f, 1.5f, 1.5f}, 0, "crate B");
  BrushPhysicsAddStaticBox(&s->phys, s->crateB2, (Vector3){1.5f, 1.5f, 1.5f}, 0, "crate B top");
  BrushPhysicsAddStaticBox(&s->phys, s->crateC, (Vector3){1.5f, 1.5f, 1.5f}, 0, "crate C");

  // Ramp (rotated slab -> triangle-MESH collider path). Visual + collision
  // share the transform.
  s->ramp = LoadModelFromMesh(GenMeshCube(6.0f, 0.4f, 10.0f));
  s->ramp.materials[0].shader = BrushGetLitShader();
  float gr = BrushWorldGroundHeight(s->world, 8.0f, -12.0f);
  s->rampPos = (Vector3){8.0f, gr + 1.55f, -12.0f};
  s->rampXform = MatrixMultiply(
      MatrixRotateX(-20.0f * DEG2RAD),
      MatrixTranslate(s->rampPos.x, s->rampPos.y, s->rampPos.z));
  BrushPhysicsAddStaticMesh(&s->phys, s->ramp.meshes[0], s->rampXform, 0, "ramp");

  // Kinematic capsule: radius/height match the mannequin (1.83 m tall).
  BrushCharacterInit(&s->body, &s->phys, s->pos, 0.30f, 1.80f);

  s->velY = 0.0f;

  BrushTodInit(&s->tod);

  BrushOrbitCamInit(&s->camera, s->pos);
  s->camera.obstructFn = CameraObstructed;
  s->camera.obstructUser = s;
  s->camera.groundHeightFn = CamGround;
  s->camera.groundHeightUser = s;

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

  // Landing harness: BRUSH_AUTO_JUMP queues a jump every 1.5s and logs the
  // capsule Y + ground state for each fixed step around the landing.
  if (getenv("BRUSH_AUTO_JUMP") != NULL) {
    static int jf = 0;
    if (++jf % 90 == 0) s->jumpQueued = true;
    TraceLog(LOG_INFO, "LANDDBG t=%d y=%+.4f velY=%+.3f grounded=%d state=%d",
             jf, s->pos.y, s->velY, s->grounded,
             (int)BrushAnimatorState(&s->animator));
  }

  // Screenshot harness: BRUSH_AUTO_MOVE=walk|jog|sprint holds forward input
  // so automated captures show the character in motion.
  const char *autoMove = getenv("BRUSH_AUTO_MOVE");
  bool autoSprint = false;
  if (autoMove != NULL) {
    inY = 1.0f;
    autoSprint = TextIsEqual(autoMove, "sprint");
    if (TextIsEqual(autoMove, "walk")) inY = 0.45f;
  }

  Vector3 camFwd = Vector3Subtract(s->camera.cam.target, s->camera.cam.position);
  camFwd.y = 0;
  camFwd = Vector3Normalize(camFwd);
  Vector3 camRight = {-camFwd.z, 0, camFwd.x};

  Vector3 wishDir = Vector3Add(Vector3Scale(camFwd, inY),
                               Vector3Scale(camRight, inX));
  float wishLen = Vector3Length(wishDir);
  if (wishLen > 1.0f) wishDir = Vector3Scale(wishDir, 1.0f / wishLen);

  s->crouched = BrushInputDown(BRUSH_BTN_CROUCH);
  if (autoMove != NULL && TextIsEqual(autoMove, "crouch")) s->crouched = true;
  float maxSpeed = (BrushInputDown(BRUSH_BTN_SPRINT) || autoSprint)
                       ? RUN_SPEED
                       : WALK_SPEED;
  if (s->crouched) maxSpeed = 1.6f; // crouch-walk pace
  Vector3 wishVel = Vector3Scale(wishDir, maxSpeed * fminf(wishLen, 1.0f));

  // Roll burst: while the roll plays, drive forward along the facing.
  if (s->rollTimer > 0.0f) {
    s->rollTimer -= dt;
    Vector3 fwd = {sinf(s->yaw), 0.0f, cosf(s->yaw)};
    wishVel = Vector3Scale(fwd, 4.2f);
  }

  // Horizontal acceleration toward the wish velocity.
  float blend = fminf(ACCEL * dt, 1.0f);
  s->vel.x += (wishVel.x - s->vel.x) * blend;
  s->vel.z += (wishVel.z - s->vel.z) * blend;

  // Jump + gravity; the kinematic capsule handles collision, wall sliding,
  // slopes, and stair steps.
  if (s->jumpQueued && s->grounded) {
    s->velY = JUMP_VELOCITY;
    s->grounded = false;
    s->airTime = 0.1f; // trip the debounced flag NOW so takeoff isn't delayed
  }
  s->jumpQueued = false;
  if (!s->grounded) s->velY -= GRAVITY * dt;
  else if (s->velY < 0.0f) s->velY = -2.0f; // gentle stick-to-floor bias

  BrushPhysicsStep(&s->phys, dt);
  BrushCharacterMove(&s->body, &s->phys,
                     (Vector3){s->vel.x, s->velY, s->vel.z}, dt);
  s->pos = s->body.position;
  s->grounded = s->body.isGrounded;
  if (s->grounded && s->velY < 0.0f) s->velY = 0.0f;
  s->vel.y = s->body.velocity.y;

  // Debounced airborne: Jolt's ground state flickers for a few frames while
  // a landing resolves; raw flag -> animator re-triggers jump each flicker.
  s->airTime = s->grounded ? 0.0f : s->airTime + dt;

  // Shared IK weight: 0 while airborne, eases back in after touchdown
  // (landing raycasts are noisy while the capsule settles). The animator
  // does the actual foot IK via the GroundUnder callback.
  s->ikWeight = s->grounded ? fminf(s->ikWeight + dt / 0.25f, 1.0f) : 0.0f;

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

  if (getenv("BRUSH_AUTO_JUMP") != NULL) {
    static int rf = 0;
    TraceLog(LOG_INFO,
             "RENDDBG rf=%d a=%.2f ry=%+.4f pelvis=%+.4f state=%d fade=%.2f",
             ++rf, alpha, s->renderPos.y, s->animator.pelvisOffset,
             (int)BrushAnimatorState(&s->animator),
             s->animator.fadeT);
  }

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

  // Roll: one-shot anim + a short forward movement burst.
  if (BrushInputPressed(BRUSH_BTN_ROLL) && s->grounded &&
      BrushAnimatorState(&s->animator) != BRUSH_ANIM_ROLL) {
    BrushAnimatorTriggerRoll(&s->animator);
    s->rollTimer = 0.55f;
  }

  // Interpolate sim state for rendering (fixed-step stutter stays invisible).
  s->renderPos = Vector3Lerp(s->prevPos, s->pos, alpha);
  float yawDiff = s->yaw - s->prevYaw;
  while (yawDiff > PI) yawDiff -= 2.0f * PI;
  while (yawDiff < -PI) yawDiff += 2.0f * PI;
  s->renderYaw = s->prevYaw + yawDiff * alpha;

  // Day/night clock drives the frame's whole lighting rig (sun/moon light,
  // ambient, sky, exposure). [ and ] scrub the clock by the hour.
  if (IsKeyDown(KEY_LEFT_BRACKET)) s->tod.timeHours -= 2.4f * dt;
  if (IsKeyDown(KEY_RIGHT_BRACKET)) s->tod.timeHours += 2.4f * dt;
  BrushTodUpdate(&s->tod, dt);
  BrushRenderApplyTimeOfDay(&s->tod);

  // Stream chunks around the player (main-thread GPU finalize happens here).
  BrushWorldUpdate(s->world, s->renderPos);

  BrushOrbitCamUpdate(&s->camera, s->renderPos, s->vel, dt);

  // Drive the animator from gameplay state (per rendered frame). The
  // airborne flag is debounced through airTime (see fixedUpdate).
  float horizSpeed = sqrtf(s->vel.x * s->vel.x + s->vel.z * s->vel.z);
  BrushAnimatorUpdate(&s->animator,
                      (BrushAnimInput){.speed = horizSpeed,
                                       .airborne = s->airTime > 0.06f,
                                       .crouched = s->crouched,
                                       .groundFn = GroundUnder,
                                       .groundUser = s,
                                       .worldPos = s->renderPos,
                                       .yawRad = s->renderYaw,
                                       .ikWeight = s->ikWeight},
                      dt);
}

// ------------------------------------------------------------------
// Draw: submit to layers, then execute the stack
// ------------------------------------------------------------------

static void SandboxDraw(void *user) {
  Sandbox *s = user;
  Vector3 p = s->renderPos;
  Matrix rot = MatrixRotateY(s->renderYaw);

  // Streamed terrain (frustum-culled chunks -> opaque + shadow layers).
  BrushWorldSubmit(s->world, s->camera.cam);

  Color crateCol = (Color){170, 120, 70, 255};
  Matrix mA = MatrixTranslate(s->crateA.x, s->crateA.y, s->crateA.z);
  Matrix mB = MatrixTranslate(s->crateB.x, s->crateB.y, s->crateB.z);
  Matrix mB2 = MatrixTranslate(s->crateB2.x, s->crateB2.y, s->crateB2.z);
  Matrix mC = MatrixTranslate(s->crateC.x, s->crateC.y, s->crateC.z);
  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->crate, mC, crateCol);
  BrushRenderSubmit(BRUSH_LAYER_SHADOW, &s->crate, mC, crateCol);
  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->crate, mA, crateCol);
  BrushRenderSubmit(BRUSH_LAYER_SHADOW, &s->crate, mA, crateCol);
  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->crate, mB, crateCol);
  BrushRenderSubmit(BRUSH_LAYER_SHADOW, &s->crate, mB, crateCol);
  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->crate, mB2, crateCol);
  BrushRenderSubmit(BRUSH_LAYER_SHADOW, &s->crate, mB2, crateCol);
  Color rampCol = (Color){120, 130, 150, 255};
  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->ramp, s->rampXform, rampCol);
  BrushRenderSubmit(BRUSH_LAYER_SHADOW, &s->ramp, s->rampXform, rampCol);

  // Skinned mannequin, feet at p (the animator already posed the meshes).
  // The body stays UPRIGHT on slopes: the animator lowered the hips to the
  // lowest foot (and the landing dip) and outputs that as pelvisOffset.
  // Submitted twice: once as scene geometry, once as a sun-shadow caster.
  Matrix xform = MatrixMultiply(
      rot, MatrixTranslate(p.x, p.y + s->animator.pelvisOffset, p.z));
  BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->mannequin, xform, WHITE);
  BrushRenderSubmit(BRUSH_LAYER_SHADOW, &s->mannequin, xform, WHITE);

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
  DrawText(TextFormat("Press [F3] to toggle HDR post: %s",
                      BrushRenderIsPostEnabled() ? "on" : "off"),
           16, 118, 20, DARKGRAY);
  DrawText(TextFormat("Press [F4] to toggle sun shadows: %s",
                      BrushRenderShadowsEnabled() ? "on" : "off"),
           16, 144, 20, DARKGRAY);
  DrawText("Hold [LCtrl] to crouch, press [R] to roll", 16, 196, 20,
           DARKGRAY);
  DrawText(TextFormat("Hold [ or ] to scrub time: %02d:%02d",
                      (int)s->tod.timeHours,
                      (int)(fmodf(s->tod.timeHours, 1.0f) * 60.0f)),
           16, 170, 20, DARKGRAY);
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
  // World holds per-chunk terrain colliders in the physics world, so it must
  // tear down (stop the worker, remove bodies) before the physics world does.
  BrushWorldDestroy(s->world);
  UnloadTexture(s->groundTex); // world doesn't own the game-supplied texture
  BrushCharacterCleanup(&s->body, &s->phys);
  BrushPhysicsCleanup(&s->phys);
  BrushAnimatorUnload(&s->animator);
  UnloadModel(s->crate);
  UnloadModel(s->ramp);
  UnloadModel(s->mannequin);
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
