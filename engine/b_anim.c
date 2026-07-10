/*******************************************************************************************
 *   b_anim.c - Skeletal character animator (see b_anim.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_anim.h"

#include <math.h>
#include <raymath.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// How fast the eased speed parameter chases the real speed (~0.25 s settle).
#define LOCO_BLEND_RATE 9.0f
// Cross-fade durations (seconds).
#define FADE_TO_JUMP 0.08f
#define FADE_TO_LAND 0.12f
#define FADE_TO_LOCO 0.15f
#define FADE_TO_CROUCH 0.18f
// Leave the land state after this fraction of the land clip (its tail is a
// slow settle that reads better blended into locomotion).
#define LAND_EXIT_PHASE 0.55f
// Above this speed (m/s) a landing skips the plant-the-feet land clip and
// cross-fades straight into locomotion — otherwise the character moonwalks
// while the clip holds its feet still.
#define LAND_RUN_THROUGH_SPEED 1.5f
// Leave the roll one-shot slightly before its end so the exit fade overlaps
// the clip's own recovery.
#define ROLL_EXIT_PHASE 0.92f

// UAL's Jump_Start opens with a crouch ANTICIPATION that launches by ~7%%
// of the clip; physics has already launched when the animator sees airborne,
// so playing the windup mid-air reads as a second dip. Enter past it (games
// trim anticipation for responsiveness).
#define JUMP_START_SKIP_PHASE 0.10f

// Landing absorption (procedural squat-and-recover on touchdown).
#define LAND_DURATION 0.40f  // seconds from impact to recovered
#define LAND_MAX_DIP 0.075f // peak pelvis drop (metres) at full strength

static const char *const UAL_CLIP_NAMES[BRUSH_CLIP_COUNT] = {
    "Idle_Loop",        "Walk_Loop",       "Jog_Fwd_Loop", "Sprint_Loop",
    "Jump_Start",       "Jump_Loop",       "Jump_Land",
    "Crouch_Idle_Loop", "Crouch_Fwd_Loop", "Roll",
};

// --- pose helpers -----------------------------------------------------------

// Per-bone blend of two poses: lerp translation/scale, slerp rotation.
static void BlendBonePoses(Transform *out, const Transform *a,
                           const Transform *b, int count, float w) {
  for (int i = 0; i < count; i++) {
    out[i].translation = Vector3Lerp(a[i].translation, b[i].translation, w);
    out[i].rotation = QuaternionSlerp(a[i].rotation, b[i].rotation, w);
    out[i].scale = Vector3Lerp(a[i].scale, b[i].scale, w);
  }
}

// Sample a clip at normalized phase [0,1], interpolating adjacent frames.
// Looping clips wrap; one-shots clamp at the last frame.
static void SampleClip(const ModelAnimation *clip, float phase, Transform *out,
                       int count, bool loop) {
  int fc = clip->frameCount;
  if (fc <= 0) return;

  float exact = phase * (float)fc;
  int f0 = (int)exact;
  int f1 = f0 + 1;
  float frac = exact - (float)f0;

  if (loop) {
    if (f0 < 0) f0 = 0;
    f0 %= fc;
    f1 %= fc;
  } else {
    if (f0 < 0) f0 = 0;
    if (f0 >= fc) f0 = fc - 1;
    if (f1 >= fc) f1 = fc - 1;
  }

  BlendBonePoses(out, clip->framePoses[f0], clip->framePoses[f1], count, frac);
}

// Cycles/sec so the clip plays at its native resampled cadence.
static float ClipRate(const ModelAnimation *clip) {
  return (clip->frameCount > 0) ? (BRUSH_ANIM_FPS / (float)clip->frameCount)
                                : 0.0f;
}

static const ModelAnimation *Clip(const BrushAnimator *a, BrushClip id) {
  int idx = a->clipIndex[id];
  return (idx >= 0) ? &a->anims[idx] : NULL;
}

// Enter a new state: snapshot the current pose as the fade source.
static void EnterState(BrushAnimator *a, BrushAnimState s, float fadeDur) {
  memcpy(a->fadePose, a->blendPose, sizeof(Transform) * (size_t)a->boneCount);
  a->state = s;
  a->phase = 0.0f;
  a->fadeT = 0.0f;
  a->fadeDur = fadeDur;
}

// raylib stores animation poses in MODEL space (the glTF loader converts
// parent-relative joint transforms to global via BuildPoseFromParentJoints),
// so blendPose translations ARE model-space joint positions, and rotating a
// bone requires rotating its whole SUBTREE about a pivot — children's global
// transforms don't follow automatically.

static bool BoneInSubtree(const Model *m, int bone, int root) {
  for (int b = bone; b >= 0; b = m->bones[b].parent)
    if (b == root) return true;
  return false;
}

// Rotate `root` and every descendant about `pivot` by `dq` (model space).
static void RotateSubtree(BrushAnimator *a, int root, Vector3 pivot,
                          Quaternion dq) {
  for (int i = 0; i < a->boneCount; i++) {
    if (!BoneInSubtree(a->model, i, root)) continue;
    Transform *t = &a->blendPose[i];
    t->translation = Vector3Add(
        pivot,
        Vector3RotateByQuaternion(Vector3Subtract(t->translation, pivot),
                                  dq));
    t->rotation = QuaternionMultiply(dq, t->rotation);
  }
}

static int FindBone(const Model *m, const char *name) {
  for (int i = 0; i < m->boneCount; i++)
    if (TextIsEqual(m->bones[i].name, name)) return i;
  return -1;
}

// --- public API -------------------------------------------------------------

bool BrushAnimatorInit(BrushAnimator *a, Model *model, const char *animFile,
                       const char *const clipNames[BRUSH_CLIP_COUNT]) {
  *a = (BrushAnimator){0};
  a->model = model;
  a->boneCount = model->boneCount;
  a->walkSpeed = 2.0f;
  a->jogSpeed = 4.5f;
  a->sprintSpeed = 7.0f;
  a->crouchSpeed = 1.6f;
  a->landDebugPin = (getenv("BRUSH_ANIM_LAND") != NULL);
  if (clipNames == NULL) clipNames = UAL_CLIP_NAMES;

  a->anims = LoadModelAnimations(animFile, &a->animCount);
  if (a->anims == NULL || a->animCount <= 0 || a->boneCount <= 0) {
    TraceLog(LOG_WARNING, "BRUSH anim: no clips in %s", animFile);
    return false;
  }

  for (int c = 0; c < BRUSH_CLIP_COUNT; c++) {
    a->clipIndex[c] = -1;
    for (int i = 0; i < a->animCount; i++) {
      if (TextIsEqual(a->anims[i].name, clipNames[c])) {
        a->clipIndex[c] = i;
        break;
      }
    }
    if (a->clipIndex[c] < 0)
      TraceLog(LOG_WARNING, "BRUSH anim: clip '%s' not found in %s",
               clipNames[c], animFile);
    else
      TraceLog(LOG_INFO, "BRUSH anim: %-16s -> '%s' (%d frames, %d bones)",
               clipNames[c], a->anims[a->clipIndex[c]].name,
               a->anims[a->clipIndex[c]].frameCount,
               a->anims[a->clipIndex[c]].boneCount);
  }
  if (a->clipIndex[BRUSH_CLIP_IDLE] < 0) return false;

  // Resolve the procedural-layer bones by name (UE-style rig names).
  a->boneSpine1 = FindBone(model, "spine_01");
  a->boneSpine2 = FindBone(model, "spine_02");
  a->bonePelvis = FindBone(model, "pelvis");
  a->boneThighL = FindBone(model, "thigh_l");
  a->boneCalfL = FindBone(model, "calf_l");
  a->boneFootL = FindBone(model, "foot_l");
  a->boneThighR = FindBone(model, "thigh_r");
  a->boneCalfR = FindBone(model, "calf_r");
  a->boneFootR = FindBone(model, "foot_r");
  if (a->boneThighL < 0 || a->boneCalfL < 0 || a->boneFootL < 0 ||
      a->boneThighR < 0 || a->boneCalfR < 0 || a->boneFootR < 0)
    TraceLog(LOG_WARNING,
             "BRUSH anim: leg bones not fully resolved — foot IK disabled "
             "(remap animator.bone* for this skeleton)");

  int bc = a->boneCount;
  a->blendPose = (Transform *)MemAlloc(sizeof(Transform) * (size_t)bc);
  a->poseA = (Transform *)MemAlloc(sizeof(Transform) * (size_t)bc);
  a->poseB = (Transform *)MemAlloc(sizeof(Transform) * (size_t)bc);
  a->fadePose = (Transform *)MemAlloc(sizeof(Transform) * (size_t)bc);

  // 1-frame animation whose pose buffer the blender fills each frame.
  a->blendAnim = (ModelAnimation){0};
  a->blendAnim.boneCount = bc;
  a->blendAnim.frameCount = 1;
  a->blendAnim.bones = a->anims[a->clipIndex[BRUSH_CLIP_IDLE]].bones;
  a->blendAnim.framePoses = &a->blendPose;

  // Start settled on idle so the first fade snapshot is valid.
  SampleClip(Clip(a, BRUSH_CLIP_IDLE), 0.0f, a->blendPose, bc, true);
  memcpy(a->fadePose, a->blendPose, sizeof(Transform) * (size_t)bc);
  a->state = BRUSH_ANIM_LOCOMOTION;
  a->fadeT = a->fadeDur = 0.0f;
  return true;
}

// Evaluate a 1-D blend over (speed, clip) anchor points at the shared gait
// phase; returns the blended cycle rate so the phase advances in cadence.
static float Eval1D(BrushAnimator *a, Transform *out,
                    const float *speeds, const BrushClip *clips, int n) {
  float s = a->speedSmooth;
  int hi = (n > 1) ? 1 : 0;
  while (hi < n - 1 && s > speeds[hi]) hi++;
  int lo = (hi > 0) ? hi - 1 : 0;
  float span = speeds[hi] - speeds[lo];
  float w = (span > 0.001f) ? (s - speeds[lo]) / span : 0.0f;
  if (w < 0.0f) w = 0.0f;
  if (w > 1.0f) w = 1.0f;

  const ModelAnimation *ca = Clip(a, clips[lo]);
  const ModelAnimation *cb = Clip(a, clips[hi]);
  if (ca == NULL) return 0.0f;
  SampleClip(ca, a->phase, a->poseA, a->boneCount, true);
  if (cb != NULL && cb != ca && w > 0.0f) {
    SampleClip(cb, a->phase, a->poseB, a->boneCount, true);
    BlendBonePoses(out, a->poseA, a->poseB, a->boneCount, w);
  } else {
    memcpy(out, a->poseA, sizeof(Transform) * (size_t)a->boneCount);
    cb = ca;
  }
  return Lerp(ClipRate(ca), ClipRate(cb), w);
}

static float EvalLocomotion(BrushAnimator *a, Transform *out) {
  float speeds[4];
  BrushClip clips[4];
  int n = 0;
  speeds[n] = 0.0f;           clips[n++] = BRUSH_CLIP_IDLE;
  if (a->clipIndex[BRUSH_CLIP_WALK] >= 0) {
    speeds[n] = a->walkSpeed;   clips[n++] = BRUSH_CLIP_WALK;
  }
  if (a->clipIndex[BRUSH_CLIP_JOG] >= 0) {
    speeds[n] = a->jogSpeed;    clips[n++] = BRUSH_CLIP_JOG;
  }
  if (a->clipIndex[BRUSH_CLIP_SPRINT] >= 0) {
    speeds[n] = a->sprintSpeed; clips[n++] = BRUSH_CLIP_SPRINT;
  }
  return Eval1D(a, out, speeds, clips, n);
}

static float EvalCrouch(BrushAnimator *a, Transform *out) {
  float speeds[2];
  BrushClip clips[2];
  int n = 0;
  speeds[n] = 0.0f;            clips[n++] = BRUSH_CLIP_CROUCH_IDLE;
  if (a->clipIndex[BRUSH_CLIP_CROUCH_WALK] >= 0) {
    speeds[n] = a->crouchSpeed;  clips[n++] = BRUSH_CLIP_CROUCH_WALK;
  }
  return Eval1D(a, out, speeds, clips, n);
}

void BrushAnimatorTriggerRoll(BrushAnimator *a) {
  if (a->blendPose == NULL) return;
  if (a->state == BRUSH_ANIM_ROLL) return;
  if (a->clipIndex[BRUSH_CLIP_ROLL] < 0) return;
  EnterState(a, BRUSH_ANIM_ROLL, 0.10f);
}

BrushAnimState BrushAnimatorState(const BrushAnimator *a) { return a->state; }

void BrushAnimatorUpdate(BrushAnimator *a, BrushAnimInput in, float dt) {
  if (a->blendPose == NULL) return;

  // Eased speed parameter so gait changes settle instead of snapping.
  a->speedSmooth +=
      (in.speed - a->speedSmooth) * fminf(LOCO_BLEND_RATE * dt, 1.0f);

  // --- transitions ---
  // Landing at speed rolls straight back into the gait; the land clip is
  // for (near-)stationary touchdowns only.
  BrushAnimState ground = in.crouched && a->clipIndex[BRUSH_CLIP_CROUCH_IDLE] >= 0
                              ? BRUSH_ANIM_CROUCH
                              : BRUSH_ANIM_LOCOMOTION;
  bool landToLoco = (in.speed > LAND_RUN_THROUGH_SPEED);
  switch (a->state) {
  case BRUSH_ANIM_LOCOMOTION:
    if (in.airborne) {
      EnterState(a, BRUSH_ANIM_JUMP_START, FADE_TO_JUMP);
      a->phase = JUMP_START_SKIP_PHASE;
    } else if (ground == BRUSH_ANIM_CROUCH)
      EnterState(a, BRUSH_ANIM_CROUCH, FADE_TO_CROUCH);
    break;
  case BRUSH_ANIM_CROUCH:
    if (in.airborne) {
      EnterState(a, BRUSH_ANIM_JUMP_START, FADE_TO_JUMP);
      a->phase = JUMP_START_SKIP_PHASE;
    } else if (ground == BRUSH_ANIM_LOCOMOTION)
      EnterState(a, BRUSH_ANIM_LOCOMOTION, FADE_TO_CROUCH);
    break;
  case BRUSH_ANIM_JUMP_START:
    if (!in.airborne)
      EnterState(a, landToLoco ? ground : BRUSH_ANIM_JUMP_LAND,
                 landToLoco ? 0.12f : FADE_TO_LAND);
    else if (a->phase >= 1.0f) EnterState(a, BRUSH_ANIM_JUMP_LOOP, 0.10f);
    break;
  case BRUSH_ANIM_JUMP_LOOP:
    if (!in.airborne)
      EnterState(a, landToLoco ? ground : BRUSH_ANIM_JUMP_LAND,
                 landToLoco ? 0.12f : FADE_TO_LAND);
    break;
  case BRUSH_ANIM_JUMP_LAND:
    if (in.airborne) {
      EnterState(a, BRUSH_ANIM_JUMP_START, FADE_TO_JUMP);
      a->phase = JUMP_START_SKIP_PHASE;
    } else if (a->phase >= LAND_EXIT_PHASE)
      EnterState(a, ground, FADE_TO_LOCO);
    break;
  case BRUSH_ANIM_ROLL:
    if (a->phase >= ROLL_EXIT_PHASE) EnterState(a, ground, FADE_TO_LOCO);
    break;
  }

  // --- evaluate the current state's pose into blendPose ---
  float rate = 0.0f;
  const ModelAnimation *clip = NULL;
  bool loop = false;
  switch (a->state) {
  case BRUSH_ANIM_LOCOMOTION:
    rate = EvalLocomotion(a, a->blendPose);
    break;
  case BRUSH_ANIM_CROUCH:
    rate = EvalCrouch(a, a->blendPose);
    break;
  case BRUSH_ANIM_JUMP_START:
    clip = Clip(a, BRUSH_CLIP_JUMP_START);
    break;
  case BRUSH_ANIM_JUMP_LOOP:
    clip = Clip(a, BRUSH_CLIP_JUMP_LOOP);
    loop = true;
    break;
  case BRUSH_ANIM_JUMP_LAND:
    clip = Clip(a, BRUSH_CLIP_JUMP_LAND);
    break;
  case BRUSH_ANIM_ROLL:
    clip = Clip(a, BRUSH_CLIP_ROLL);
    break;
  }
  if (clip != NULL) {
    SampleClip(clip, a->phase, a->blendPose, a->boneCount, loop);
    rate = ClipRate(clip);
  } else if (a->state != BRUSH_ANIM_LOCOMOTION &&
             a->state != BRUSH_ANIM_CROUCH) {
    // Missing clip: fall back to locomotion so the character never freezes.
    rate = EvalLocomotion(a, a->blendPose);
  }

  // Advance the phase; loops wrap, one-shots clamp just past the end so
  // "finished" transitions fire.
  a->phase += rate * dt;
  bool looping = (a->state == BRUSH_ANIM_LOCOMOTION ||
                  a->state == BRUSH_ANIM_CROUCH ||
                  a->state == BRUSH_ANIM_JUMP_LOOP);
  if (looping) {
    while (a->phase >= 1.0f) a->phase -= 1.0f;
  } else if (a->phase > 1.0f) {
    a->phase = 1.0f;
  }

  // --- cross-fade from the snapshot taken at the last state change ---
  if (a->fadeT < a->fadeDur) {
    a->fadeT += dt;
    float t = (a->fadeDur > 0.0f) ? a->fadeT / a->fadeDur : 1.0f;
    if (t > 1.0f) t = 1.0f;
    t = t * t * (3.0f - 2.0f * t); // smoothstep
    BlendBonePoses(a->blendPose, a->fadePose, a->blendPose, a->boneCount, t);
  }

  // --- inertia leans: the body banks into acceleration and turns ----------
  // The gait blend alone changes leg speed with no sense of mass; leaning the
  // spine into acceleration (and slightly back on braking) plus into turns
  // supplies the missing inertia.
  {
    float accel = (a->speedSmooth - a->prevSpeedSmooth) / fmaxf(dt, 1e-4f);
    a->prevSpeedSmooth = a->speedSmooth;
    float yawDelta = in.yawRad - a->prevYawRad;
    while (yawDelta > PI) yawDelta -= 2.0f * PI;
    while (yawDelta < -PI) yawDelta += 2.0f * PI;
    a->prevYawRad = in.yawRad;
    float turnRate = yawDelta / fmaxf(dt, 1e-4f);

    float leanTargetF =
        in.airborne ? 0.0f : Clamp(accel * 0.022f, -0.10f, 0.14f);
    float leanTargetS =
        in.airborne ? 0.0f
                    : Clamp(turnRate * 0.05f *
                                fminf(a->speedSmooth / 6.0f, 1.0f),
                            -0.12f, 0.12f);
    float leanBlend = 1.0f - expf(-8.0f * dt);
    a->leanFwd += (leanTargetF - a->leanFwd) * leanBlend;
    a->leanSide += (leanTargetS - a->leanSide) * leanBlend;

    if (a->boneSpine1 >= 0 &&
        (fabsf(a->leanFwd) > 0.004f || fabsf(a->leanSide) > 0.004f)) {
      // Distribute the bend over two spine joints for a curve, not a hinge.
      Quaternion qLean = QuaternionMultiply(
          QuaternionFromAxisAngle((Vector3){1, 0, 0}, a->leanFwd * 0.6f),
          QuaternionFromAxisAngle((Vector3){0, 0, 1}, -a->leanSide * 0.6f));
      RotateSubtree(a, a->boneSpine1,
                    a->blendPose[a->boneSpine1].translation, qLean);
      if (a->boneSpine2 >= 0) {
        Quaternion qLean2 = QuaternionMultiply(
            QuaternionFromAxisAngle((Vector3){1, 0, 0}, a->leanFwd * 0.4f),
            QuaternionFromAxisAngle((Vector3){0, 0, 1}, -a->leanSide * 0.4f));
        RotateSubtree(a, a->boneSpine2,
                      a->blendPose[a->boneSpine2].translation, qLean2);
      }
    }
  }

  // --- landing absorption (procedural squat-and-recover on touchdown) ------
  // Edge-detect airborne->grounded and fire a short dip: the pelvis drops and
  // both knees bend (via the foot IK below, so the feet stay planted), then
  // eases back. This is the weight/impact a straight cross-fade back to
  // locomotion otherwise lacks. Heavier when landing at speed.
  bool landed = a->prevAirborne && !in.airborne;
  a->prevAirborne = in.airborne;
  // Only fire the procedural dip when the landing skipped the land CLIP
  // (running landings roll straight into locomotion). The clip contains its
  // own crouch-and-recover — stacking the dip on top reads as two landing
  // animations playing at once.
  if (landed && a->state != BRUSH_ANIM_JUMP_LAND) {
    a->landTimer = LAND_DURATION;
    a->landStrength = 0.6f + 0.4f * Clamp(in.speed / 6.0f, 0.0f, 1.0f);
  }
  float landDip = 0.0f;
  if (a->landTimer > 0.0f) {
    float p = 1.0f - a->landTimer / LAND_DURATION; // 0 at contact -> 1 done
    // Fast onset (knees give immediately), slower recovery.
    float shape = sinf(sqrtf(Clamp(p, 0.0f, 1.0f)) * PI);
    landDip = shape * a->landStrength * LAND_MAX_DIP;
    a->landTimer -= dt;
    if (a->landTimer < 0.0f) a->landTimer = 0.0f;
  }
  if (a->landDebugPin) landDip = LAND_MAX_DIP; // hold for a screenshot
  // landDip feeds pelvisOffset below; the foot IK keeps the feet planted,
  // which is what bends the knees during the dip.

  // --- foot IK: the standard pipeline -------------------------------------
  // Rays under the ANIMATED feet -> pelvis lowers to the lowest reachable
  // foot -> analytic two-bone IK drives each ankle to its exact target ->
  // ankles aim to the ground normal. All math in model space on the GLOBAL
  // bone poses raylib stores (see RotateSubtree).
  float w = Clamp(in.ikWeight, 0.0f, 1.0f);
  bool haveLegs = a->boneThighL >= 0 && a->boneCalfL >= 0 &&
                  a->boneFootL >= 0 && a->boneThighR >= 0 &&
                  a->boneCalfR >= 0 && a->boneFootR >= 0;
  float targetDL = 0.0f, targetDR = 0.0f;
  Vector3 normL = {0, 1, 0}, normR = {0, 1, 0};
  if (haveLegs && in.groundFn != NULL && w > 0.001f) {
    // Ground plane under the character (the model origin's ground).
    float baseY = in.worldPos.y;
    float gy;
    if (in.groundFn(in.groundUser, in.worldPos, &gy, NULL)) baseY = gy;

    float cy = cosf(in.yawRad), sy = sinf(in.yawRad);
    for (int side = 0; side < 2; side++) {
      int footBone = side == 0 ? a->boneFootL : a->boneFootR;
      Vector3 ankle = a->blendPose[footBone].translation;
      // Model space -> world (yaw about Y, then translate).
      Vector3 probe = {
          in.worldPos.x + ankle.x * cy + ankle.z * sy,
          in.worldPos.y + ankle.y,
          in.worldPos.z - ankle.x * sy + ankle.z * cy,
      };
      float hitY;
      Vector3 n = {0, 1, 0};
      float d = 0.0f;
      if (in.groundFn(in.groundUser, probe, &hitY, &n)) {
        d = hitY - baseY;
        // Ledge rejection: ground far below the foot (overhanging an edge)
        // is unreachable — chasing it drags the pelvis down and buries the
        // planted foot. Ignore it instead of clamping toward it.
        if (d < -0.45f) d = 0.0f;
        d = Clamp(d, -0.45f, 0.5f) * w;
      }
      if (side == 0) { targetDL = d; normL = n; }
      else { targetDR = d; normR = n; }
    }
  }
  // Smooth the terrain deltas so steps/edges ease instead of popping.
  float ikBlend = 1.0f - expf(-15.0f * dt);
  a->footDeltaL += (targetDL - a->footDeltaL) * ikBlend;
  a->footDeltaR += (targetDR - a->footDeltaR) * ikBlend;

  // Pelvis follows the LOWEST foot (never rises above the animation), and
  // the landing dip rides on top; the IK below keeps the feet planted, so
  // the dip is what bends the knees.
  float pelvis = fminf(0.0f, fminf(a->footDeltaL, a->footDeltaR));
  pelvis = fmaxf(pelvis, -0.30f);
  a->pelvisOffset = pelvis - landDip;

  if (haveLegs && (in.groundFn != NULL || landDip > 0.0001f)) {
    for (int side = 0; side < 2; side++) {
      int thighB = side == 0 ? a->boneThighL : a->boneThighR;
      int calfB = side == 0 ? a->boneCalfL : a->boneCalfR;
      int footB = side == 0 ? a->boneFootL : a->boneFootR;
      float delta = side == 0 ? a->footDeltaL : a->footDeltaR;
      Vector3 groundN = side == 0 ? normL : normR;

      Vector3 H = a->blendPose[thighB].translation; // hip joint
      Vector3 K = a->blendPose[calfB].translation;  // knee joint
      Vector3 A = a->blendPose[footB].translation;  // ankle joint
      float L1 = Vector3Distance(H, K);
      float L2 = Vector3Distance(K, A);
      if (L1 < 0.01f || L2 < 0.01f) continue;

      // Ankle target: the animated ankle raised/lowered by the terrain
      // delta, compensated for the root shift the game will apply.
      Vector3 T = {A.x, A.y + delta - a->pelvisOffset, A.z};
      float reach = Clamp(Vector3Distance(H, T), fabsf(L1 - L2) + 0.01f,
                          L1 + L2 - 0.01f);
      if (Vector3Distance(A, T) < 0.002f) continue; // nothing to solve

      // 1. Knee angle from the law of cosines, rotating the calf subtree
      //    about the knee-plane normal (preserves the knee direction).
      Vector3 u = Vector3Subtract(H, K);
      Vector3 v = Vector3Subtract(A, K);
      Vector3 kneeAxis = Vector3CrossProduct(v, u);
      if (Vector3Length(kneeAxis) < 1e-5f) kneeAxis = (Vector3){1, 0, 0};
      kneeAxis = Vector3Normalize(kneeAxis);
      float curKnee = acosf(
          Clamp(Vector3DotProduct(Vector3Normalize(u), Vector3Normalize(v)),
                -1.0f, 1.0f));
      float wantKnee = acosf(
          Clamp((L1 * L1 + L2 * L2 - reach * reach) / (2.0f * L1 * L2),
                -1.0f, 1.0f));
      // Rotating v=(A-K) about cross(v,u) by the angle between them brings v
      // onto u, so (curKnee - wantKnee) moves the interior angle exactly to
      // wantKnee.
      Quaternion qKnee =
          QuaternionFromAxisAngle(kneeAxis, curKnee - wantKnee);
      RotateSubtree(a, calfB, K, qKnee);
      // Safety net for degenerate axes: if the angle still moved the wrong
      // way, reverse — and FOLD THE CORRECTION INTO qKnee so the ankle
      // counter-rotation below stays exact.
      Vector3 A1 = a->blendPose[footB].translation;
      float gotKnee = acosf(Clamp(
          Vector3DotProduct(
              Vector3Normalize(Vector3Subtract(H, K)),
              Vector3Normalize(Vector3Subtract(A1, K))),
          -1.0f, 1.0f));
      if (fabsf(gotKnee - wantKnee) > 0.01f + fabsf(curKnee - wantKnee)) {
        Quaternion qFixup = QuaternionFromAxisAngle(
            kneeAxis, 2.0f * (wantKnee - curKnee));
        RotateSubtree(a, calfB, K, qFixup);
        qKnee = QuaternionMultiply(qFixup, qKnee);
        A1 = a->blendPose[footB].translation;
      }

      // 2. Swing the whole leg at the hip so the ankle lands on the target.
      Vector3 fromDir = Vector3Normalize(Vector3Subtract(A1, H));
      Vector3 toDir = Vector3Normalize(Vector3Subtract(T, H));
      Quaternion qHip = QuaternionFromVector3ToVector3(fromDir, toDir);
      RotateSubtree(a, thighB, H, qHip);
      Vector3 A2 = a->blendPose[footB].translation;

      // 3. Counter-rotate the ankle subtree so the foot keeps its authored
      //    orientation, then aim it to the ground normal (capped+weighted).
      Quaternion qNet = QuaternionMultiply(qHip, qKnee);
      RotateSubtree(a, footB, A2, QuaternionInvert(qNet));
      if (w > 0.001f) {
        float cy2 = cosf(in.yawRad), sy2 = sinf(in.yawRad);
        Vector3 nM = {groundN.x * cy2 - groundN.z * sy2, groundN.y,
                      groundN.x * sy2 + groundN.z * cy2};
        Quaternion qAim =
            QuaternionFromVector3ToVector3((Vector3){0, 1, 0}, nM);
        float ang = 2.0f * acosf(Clamp(fabsf(qAim.w), -1.0f, 1.0f));
        if (ang > 0.45f)
          qAim = QuaternionSlerp(QuaternionIdentity(), qAim, 0.45f / ang);
        qAim = QuaternionSlerp(QuaternionIdentity(), qAim, w);
        RotateSubtree(a, footB, A2, qAim);
      }
    }
  }

  UpdateModelAnimation(*a->model, a->blendAnim, 0);
}

void BrushAnimatorUnload(BrushAnimator *a) {
  if (a == NULL) return;
  if (a->anims != NULL) UnloadModelAnimations(a->anims, a->animCount);
  if (a->blendPose) MemFree(a->blendPose);
  if (a->poseA) MemFree(a->poseA);
  if (a->poseB) MemFree(a->poseB);
  if (a->fadePose) MemFree(a->fadePose);
  *a = (BrushAnimator){0};
}
