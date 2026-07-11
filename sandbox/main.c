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
#include <string.h>
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

  // Mantle: jump at a waist-to-chest-high ledge -> ClimbUp_1m one-shot while
  // the capsule is scripted from mantleFrom onto mantleTo.
  float mantleTimer, mantleDur; // seconds remaining / total (0 = not mantling)
  Vector3 mantleFrom, mantleTo;

  // Stair-step render smoothing: Jolt teleports the capsule a full riser
  // within one fixed step; this offset absorbs the pop and decays so the
  // visual body glides while foot IK keeps the feet on the treads.
  float stepSmooth, prevStepSmooth;

  BrushWorld *world; // chunk-streamed terrain (flat by default)
  Texture2D groundTex;

  // Blockout gym: pure DATA in a world.def scene (assets/gym.def) — blocks,
  // torches, spawn, time. First run bootstraps the file from BuildGymScene;
  // afterwards it loads (and HOT-RELOADS: edit the file while running). One
  // shared unit cube draws every block; colliders are applied from the data.
  BrushScene scene;
  JPH_BodyID blockBodies[BRUSH_SCENE_MAX_BLOCKS];
  int blockBodyCount;
  Model unitCube;

  // Project this player is running (see b_project.h): the process cwd is
  // the project root, scenePath/terrainPath come from project.def.
  BrushProject project;
  bool projectLoaded;
  char scenePath[BRUSH_PROJECT_SCENE_MAX];
  char terrainPath[BRUSH_PROJECT_SCENE_MAX];


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

// Add a gym block: visual (drawn from the shared unit cube) + box collider.
static void AddBlock(Sandbox *s, Vector3 pos, Vector3 size, Color color) {
  BrushScene *sc = &s->scene;
  if (sc->blockCount >= BRUSH_SCENE_MAX_BLOCKS) return;
  sc->blocks[sc->blockCount++] =
      (BrushSceneBlock){.pos = pos, .size = size, .color = color};
}

// The gym as DATA (see world.def notes in b_scene.h). Runs once to bootstrap
// assets/gym.def; afterwards the file is the authority and this is only a
// fallback. gy = ground height under the gym.
static void BuildGymScene(Sandbox *s, float gy) {
  Color wallCol = (Color){168, 172, 180, 255};
  Color platCol = (Color){140, 146, 158, 255};
  Color stepCol = (Color){110, 130, 168, 255};
  Color crateCol = (Color){170, 120, 70, 255};

  s->scene.spawn = (Vector3){0, gy + 0.2f, 0};
  s->scene.timeHours = 10.5f;

  // Arena walls (x in [-14,14], z in [-26,-2]), opening on the south side.
  AddBlock(s, (Vector3){0, gy + 1.5f, -26}, (Vector3){28.6f, 3, 0.6f}, wallCol);
  AddBlock(s, (Vector3){-9.25f, gy + 1.5f, -2}, (Vector3){10.1f, 3, 0.6f}, wallCol);
  AddBlock(s, (Vector3){9.25f, gy + 1.5f, -2}, (Vector3){10.1f, 3, 0.6f}, wallCol);
  AddBlock(s, (Vector3){-14, gy + 1.5f, -14}, (Vector3){0.6f, 3, 24.6f}, wallCol);
  AddBlock(s, (Vector3){14, gy + 1.5f, -14}, (Vector3){0.6f, 3, 24.6f}, wallCol);

  // High platform (top at 2 m) + low platform (top at 1 m) beside it.
  AddBlock(s, (Vector3){9, gy + 1.0f, -21}, (Vector3){8, 2, 7}, platCol);
  AddBlock(s, (Vector3){2.5f, gy + 0.5f, -21}, (Vector3){5, 1, 5}, platCol);

  // Staircase onto the low platform (0.33 m risers — Jolt stair-step range).
  AddBlock(s, (Vector3){2.5f, gy + 0.165f, -17.2f}, (Vector3){3, 0.33f, 0.9f}, stepCol);
  AddBlock(s, (Vector3){2.5f, gy + 0.33f, -18.0f}, (Vector3){3, 0.66f, 0.9f}, stepCol);

  // Stacked crates for jump tests + a lone one inside the arena.
  AddBlock(s, (Vector3){-4, gy + 0.75f, -5}, (Vector3){1.5f, 1.5f, 1.5f}, crateCol);
  AddBlock(s, (Vector3){-4, gy + 2.25f, -5}, (Vector3){1.5f, 1.5f, 1.5f}, crateCol);
  AddBlock(s, (Vector3){-9, gy + 0.75f, -20}, (Vector3){1.5f, 1.5f, 1.5f}, crateCol);

  // Torches: entrance pair + crate stack (warm HDR color, flickering).
  Vector3 torchCol = {2.4f, 1.25f, 0.45f};
  s->scene.lights[0] = (BrushSceneLight){
      .light = {{-4.6f, gy + 2.4f, -2.3f}, torchCol, 9.0f}, .flicker = true};
  s->scene.lights[1] = (BrushSceneLight){
      .light = {{4.6f, gy + 2.4f, -2.3f}, torchCol, 9.0f}, .flicker = true};
  s->scene.lights[2] = (BrushSceneLight){
      .light = {{-4.0f, gy + 3.4f, -5.0f}, torchCol, 9.0f}, .flicker = true};
  s->scene.lightCount = 3;
}

// (Re)create the box colliders from the scene data — called after every
// load/hot-reload so physics always matches the file.
static void ApplySceneColliders(Sandbox *s) {
  for (int i = 0; i < s->blockBodyCount; i++)
    BrushPhysicsRemoveBody(&s->phys, s->blockBodies[i]);
  s->blockBodyCount = 0;
  for (int i = 0; i < s->scene.blockCount; i++) {
    BrushSceneBlock *k = &s->scene.blocks[i];
    s->blockBodies[s->blockBodyCount++] =
        BrushPhysicsAddStaticBox(&s->phys, k->pos, k->size, k->rot, i, "block");
  }
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

  // World definition from the project's main scene (data, not code).
  bool sceneLoaded = BrushSceneLoad(&s->scene, s->scenePath);

  // Spawn: BRUSH_SPAWN env > scene file > origin.
  float spx = s->scene.spawn.x, spz = s->scene.spawn.z;
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

  // Terrain sculpt overlay (authored in the editor, saved beside the scene).
  BrushWorldSculptLoad(s->world, s->terrainPath);

  // Harness: BRUSH_TEST_SCULPT raises a hill ahead of spawn — verifies the
  // sculpt compose, dirty-chunk rebake, and collider swap without the editor.
  if (getenv("BRUSH_TEST_SCULPT") != NULL)
    for (int i = 0; i < 40; i++)
      BrushWorldSculpt(s->world, BRUSH_SCULPT_ADD, (Vector3){0, 0, 14}, 9.0f,
                       0.09f, 0.0f);

  float groundY = BrushWorldGroundHeight(s->world, spx, spz);
  s->pos = s->prevPos = s->renderPos = (Vector3){spx, groundY + 0.2f, spz};
  s->grounded = true;

  // --- Blockout gym (UE third-person-template style): a walled arena with
  // platforms at two heights, a ramp chain, a staircase, and stacked crates.
  // Every piece exercises a character system: stairs -> Jolt stair-step,
  // ramp -> slope IK, platforms -> jumps and ledge handling. Flat terrain by
  // default puts everything at ground level; with BRUSH_HILLS the ground
  // height query seats the gym on the terrain.
  s->unitCube = LoadModelFromMesh(GenMeshCube(1.0f, 1.0f, 1.0f));
  s->unitCube.materials[0].shader = BrushGetLitShader();
  float gy = BrushWorldGroundHeight(s->world, 0.0f, -14.0f);

  // Bootstrap: no def file yet -> build the gym in code and SAVE it. From
  // then on the file is the authority (and hot-reloads while running).
  if (!sceneLoaded) {
    if (s->projectLoaded) {
      // A declared project with no scene yet (Empty template before the
      // editor's first save): minimal spawn-only scene, not the gym.
      s->scene.spawn = (Vector3){0, 0.5f, 8};
      s->scene.timeHours = 12.0f;
    } else {
      BuildGymScene(s, gy);
    }
    BrushSceneSave(&s->scene, s->scenePath);
  }
  ApplySceneColliders(s);
  BrushSceneApplyRenderSettings(&s->scene); // scene carries its look


  // Harness: BRUSH_TEST_TRIGGER drops a big sensor volume across the
  // default camera boom — raycasts (camera anti-clip, IK probes) must see
  // straight through it, and the capsule must walk through it.
  if (getenv("BRUSH_TEST_TRIGGER") != NULL)
    BrushPhysicsAddTriggerBox(&s->phys, (Vector3){0, gy + 1.5f, 6},
                              (Vector3){8, 3, 2}, 1, "test_trigger");

  // Kinematic capsule: radius/height match the mannequin (1.83 m tall).
  BrushCharacterInit(&s->body, &s->phys, s->pos, 0.30f, 1.80f);

  s->velY = 0.0f;

  BrushTodInit(&s->tod);
  // Scene file sets the starting clock; BRUSH_TIME env still wins.
  if (s->scene.timeHours >= 0.0f && getenv("BRUSH_TIME") == NULL)
    s->tod.timeHours = s->scene.timeHours;

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
  s->prevStepSmooth = s->stepSmooth;

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

  // Roll harness: BRUSH_AUTO_ROLL triggers a roll every 2.5 s and logs the
  // anim state + capsule speed around it (moonwalk = high speed in ROLL).
  if (getenv("BRUSH_AUTO_ROLL") != NULL) {
    static int rollF = 0;
    if (++rollF % 150 == 0 && s->grounded &&
        BrushAnimatorState(&s->animator) != BRUSH_ANIM_ROLL)
      BrushAnimatorTriggerRoll(&s->animator);
    if (rollF % 6 == 0) {
      float sp = sqrtf(s->vel.x * s->vel.x + s->vel.z * s->vel.z);
      TraceLog(LOG_INFO, "ROLLDBG t=%.2f state=%d phase=%.2f speed=%.2f",
               rollF / 60.0f, (int)BrushAnimatorState(&s->animator),
               BrushAnimatorPhase(&s->animator), sp);
    }
  }

  // Traversal harness: BRUSH_POS_DBG logs the capsule position twice a
  // second — pair with BRUSH_SPAWN + BRUSH_AUTO_MOVE to verify stairs,
  // ramps, and ledges without eyeballing screenshots.
  if (getenv("BRUSH_POS_DBG") != NULL) {
    static int pf = 0;
    if (++pf % 30 == 0)
      TraceLog(LOG_INFO,
               "POSDBG t=%.1f pos=(%+.2f %+.2f %+.2f) grounded=%d step=%+.3f",
               pf / 60.0f, s->pos.x, s->pos.y, s->pos.z, s->grounded,
               s->stepSmooth);
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

  // --- Mantle in progress: the capsule is scripted, not simulated. The
  // path is a straight LERP because that is what the clip was authored
  // against: ClimbUp_1m's root-motion variant bakes a linear root ramp
  // (measured in Blender: 1.68 m forward, 1.0 m up, constant rate) — any
  // shaped path desyncs the body from the reaching hands/stepping feet.
  // IK stays off (the clip owns the feet).
  if (s->mantleTimer > 0.0f) {
    s->mantleTimer -= dt;
    float t = 1.0f - fmaxf(s->mantleTimer, 0.0f) / s->mantleDur;
    Vector3 p = Vector3Lerp(s->mantleFrom, s->mantleTo, t);
    BrushPhysicsStep(&s->phys, dt);
    BrushCharacterWarp(&s->body, p);
    s->pos = p;
    s->vel = (Vector3){0};
    s->velY = 0.0f;
    s->grounded = true;
    s->airTime = 0.0f;
    s->ikWeight = 0.0f;
    s->stepSmooth = 0.0f; // the scripted ride is already smooth
    s->jumpQueued = false;
    return;
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

  // Roll burst: while the animator owns the pose, the CAPSULE follows the
  // clip's pacing — full speed through the tumble, tapering into the
  // stand-up. Input never drives the capsule mid-roll (a timer-based burst
  // left a half-second gap where held input ran at sprint speed under a
  // rolling pose — the classic moonwalk). Holding a direction keeps enough
  // speed for the animator's movement-cancel to fire at its earliest phase.
  if (BrushAnimatorState(&s->animator) == BRUSH_ANIM_ROLL) {
    float ph = BrushAnimatorPhase(&s->animator);
    float burst = 4.2f * fminf(fmaxf((0.78f - ph) / 0.30f, 0.0f), 1.0f);
    if (wishLen > 0.1f && burst < 2.2f) burst = 2.2f; // arm the run-cancel
    Vector3 fwd = {sinf(s->yaw), 0.0f, cosf(s->yaw)};
    wishVel = Vector3Scale(fwd, burst);
  }

  // Horizontal acceleration toward the wish velocity.
  float blend = fminf(ACCEL * dt, 1.0f);
  s->vel.x += (wishVel.x - s->vel.x) * blend;
  s->vel.z += (wishVel.z - s->vel.z) * blend;

  // --- Mantle detection: jumping while facing a waist-to-chest-high ledge
  // climbs it instead (UAL2's ClimbUp_1m). Chest ray finds the face, a probe
  // over the lip finds a standable top; taller obstacles (walls, stacked
  // crates) reject on the rise limit and fall through to a normal jump.
  if (s->jumpQueued && s->grounded) {
    Vector3 fwd = {sinf(s->yaw), 0.0f, cosf(s->yaw)};
    Vector3 chest = Vector3Add(s->pos, (Vector3){0.0f, 0.7f, 0.0f});
    Vector3 wallHit, wallN;
    if (BrushPhysicsRaycast(&s->phys, chest, fwd, 0.9f, &wallHit, &wallN)) {
      // Into-the-wall direction from the face normal (not the approach
      // heading): the clip climbs square-on, so an oblique jump must snap
      // the yaw to the face or the hands miss sideways.
      Vector3 into = Vector3Normalize((Vector3){-wallN.x, 0.0f, -wallN.z});
      Vector3 over = {wallHit.x + into.x * 0.35f, s->pos.y + 1.5f,
                      wallHit.z + into.z * 0.35f};
      Vector3 topHit;
      if (BrushPhysicsRaycast(&s->phys, over, (Vector3){0, -1, 0}, 1.5f,
                              &topHit, NULL)) {
        float rise = topHit.y - s->pos.y;
        if (rise > 0.5f && rise < 1.2f) {
          float clipDur =
              BrushAnimatorPlayOneShot(&s->animator, "ClimbUp_1m", 0.92f);
          if (clipDur > 0.0f) {
            s->mantleDur = clipDur * 0.92f; // ride matches the played span
            s->mantleTimer = s->mantleDur;
            s->mantleFrom = s->pos;
            // Land a fixed 0.4 m past the face (capsule radius + margin);
            // height from the top probe.
            s->mantleTo = (Vector3){wallHit.x + into.x * 0.4f, topHit.y,
                                    wallHit.z + into.z * 0.4f};
            s->yaw = atan2f(into.x, into.z);
            s->jumpQueued = false;
          }
        }
      }
    }
  }

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
  // a landing resolves (and mid-riser on stairs); raw flag -> animator
  // re-triggers jump each flicker. Every gate below uses the DEBOUNCED
  // signal — raw-grounded gates flap on stair flickers and read as stutter.
  float prevAirTime = s->airTime;
  s->airTime = s->grounded ? 0.0f : s->airTime + dt;
  bool solid = s->airTime < 0.06f;

  // Stair-step smoothing: on stairs the capsule's Y is a sawtooth — Jolt's
  // stair-walk spreads each riser over a few steps going up, and stick-to-
  // floor drops a whole riser in ONE step going down — and following it
  // exactly reads as bouncing. Low-pass grounded Y motion into a decaying
  // render offset, but ONLY while the ground BELOW is FLAT (that's the
  // stairs/ramp discriminator: slopes must track exactly — smoothing them
  // sinks ascents and floats descents). The flatness ray probes straight
  // down: the capsule's own contact normal tilts whenever it rides a tread
  // EDGE, flapping a per-step gate. Landing edges are skipped (prevAirTime
  // check); real airborne kills the offset fast.
  float stepDy = s->pos.y - s->prevPos.y;
  if (solid && prevAirTime < 0.06f) {
    float underY;
    Vector3 underN;
    if (GroundUnder(s, s->pos, &underY, &underN) && underN.y > 0.98f)
      s->stepSmooth -= stepDy;
  }
  s->stepSmooth = fminf(fmaxf(s->stepSmooth, -0.30f), 0.30f);
  s->stepSmooth *= expf((solid ? -8.0f : -30.0f) * dt);

  // Shared IK weight: 0 while airborne, eases back in after touchdown
  // (landing raycasts are noisy while the capsule settles). The animator
  // does the actual foot IK via the GroundUnder callback.
  s->ikWeight = solid ? fminf(s->ikWeight + dt / 0.25f, 1.0f) : 0.0f;

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

  if (getenv("BRUSH_AUTO_JUMP") != NULL || getenv("BRUSH_REND_DBG") != NULL) {
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
      BrushAnimatorState(&s->animator) != BRUSH_ANIM_ROLL)
    BrushAnimatorTriggerRoll(&s->animator);

  // Interpolate sim state for rendering (fixed-step stutter stays invisible).
  // stepSmooth rides on Y: everything downstream (draw transform, camera
  // focus, foot-IK probe origin) sees the glided body, so the feet re-plant
  // on the real treads via IK while the body eases between them.
  s->renderPos = Vector3Lerp(s->prevPos, s->pos, alpha);
  s->renderPos.y +=
      s->prevStepSmooth + (s->stepSmooth - s->prevStepSmooth) * alpha;
  float yawDiff = s->yaw - s->prevYaw;
  while (yawDiff > PI) yawDiff -= 2.0f * PI;
  while (yawDiff < -PI) yawDiff += 2.0f * PI;
  s->renderYaw = s->prevYaw + yawDiff * alpha;

  // world.def hot reload: edit assets/gym.def in any editor while running —
  // blocks, lights, and colliders re-apply within half a second.
  static int reloadPoll = 0;
  if (++reloadPoll >= 30) {
    reloadPoll = 0;
    if (BrushSceneHotReload(&s->scene)) {
      ApplySceneColliders(s);
      BrushSceneApplyRenderSettings(&s->scene);
    }
  }

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

  // Scene blocks (world.def data): one shared unit cube, scaled per block.
  // Materials ride as per-draw props (triplanar, so scaling doesn't stretch).
  for (int i = 0; i < s->scene.blockCount; i++) {
    BrushSceneBlock *k = &s->scene.blocks[i];
    Matrix xf = BrushBlockGetModelMatrix(k);
    BrushMaterialProps props;
    bool hasMat = BrushSceneBlockProps(&s->scene, k, &props);
    BrushRenderSubmitEx(BRUSH_LAYER_OPAQUE, &s->unitCube, xf, k->color,
                        hasMat ? &props : NULL);
    BrushRenderSubmit(BRUSH_LAYER_SHADOW, &s->unitCube, xf, k->color);
  }

  // Scene lights, submitted per frame like draws. Flickering ones get two
  // detuned sines; colors are linear HDR (>1 blooms).
  float tt = (float)GetTime();
  for (int i = 0; i < s->scene.lightCount; i++) {
    BrushSceneLight *l = &s->scene.lights[i];
    float fl = 1.0f;
    if (l->flicker)
      fl = 0.85f + 0.11f * sinf(tt * 9.0f + (float)i * 2.1f) +
           0.06f * sinf(tt * 23.7f + (float)i * 4.3f);
    BrushPointLight pl = l->light;
    pl.color = (Vector3){pl.color.x * fl, pl.color.y * fl, pl.color.z * fl};
    BrushRenderSubmitPointLight(pl);
    // Small marker cube so the source reads in-scene.
    Matrix mk = MatrixMultiply(
        MatrixScale(0.16f, 0.16f, 0.16f),
        MatrixTranslate(l->light.position.x, l->light.position.y,
                        l->light.position.z));
    BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &s->unitCube, mk,
                      (Color){255, 190, 90, 255});
  }

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
  DrawText("Hold [LCtrl] to crouch, press [R] to roll, jump at a ledge to climb", 16, 196, 20,
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
  UnloadModel(s->unitCube);

  UnloadModel(s->mannequin);
  BrushSceneUnloadMaterials(&s->scene);
  BrushAssetsShutdown();
}

int main(int argc, char **argv) {
  static Sandbox s = {0};

  // Enter the project (--project <dir> / BRUSH_PROJECT env / cwd) before
  // anything opens files: from here on the cwd IS the project root.
  s.projectLoaded = BrushProjectBoot(&s.project, argc, argv);
  snprintf(s.scenePath, sizeof(s.scenePath), "%s", s.project.scene);
  // Terrain sculpt overlay lives beside the scene: <scene>.terrain.
  snprintf(s.terrainPath, sizeof(s.terrainPath), "%s", s.scenePath);
  char *dot = strrchr(s.terrainPath, '.');
  if (dot != NULL && dot != s.terrainPath) *dot = '\0';
  strncat(s.terrainPath, ".terrain",
          sizeof(s.terrainPath) - strlen(s.terrainPath) - 1);

  BrushRun(
      (BrushConfig){.width = 1600, .height = 900,
                    .title = s.projectLoaded ? s.project.name
                                             : "brush sandbox"},
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
