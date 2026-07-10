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

// Landing absorption (procedural squat-and-recover on touchdown).
#define LAND_DURATION 0.40f  // seconds from impact to recovered
#define LAND_MAX_DIP 0.075f  // peak pelvis drop (metres) at full strength
#define LAND_KNEE_RATIO 2.2f // foot-IK delta per metre of dip (feet planted)

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
    if (in.airborne) EnterState(a, BRUSH_ANIM_JUMP_START, FADE_TO_JUMP);
    else if (ground == BRUSH_ANIM_CROUCH)
      EnterState(a, BRUSH_ANIM_CROUCH, FADE_TO_CROUCH);
    break;
  case BRUSH_ANIM_CROUCH:
    if (in.airborne) EnterState(a, BRUSH_ANIM_JUMP_START, FADE_TO_JUMP);
    else if (ground == BRUSH_ANIM_LOCOMOTION)
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
    if (in.airborne) EnterState(a, BRUSH_ANIM_JUMP_START, FADE_TO_JUMP);
    else if (a->phase >= LAND_EXIT_PHASE) EnterState(a, ground, FADE_TO_LOCO);
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

  // --- landing absorption (procedural squat-and-recover on touchdown) ------
  // Edge-detect airborne->grounded and fire a short dip: the pelvis drops and
  // both knees bend (via the foot IK below, so the feet stay planted), then
  // eases back. This is the weight/impact a straight cross-fade back to
  // locomotion otherwise lacks. Heavier when landing at speed.
  bool landed = a->prevAirborne && !in.airborne;
  a->prevAirborne = in.airborne;
  if (landed) {
    a->landTimer = LAND_DURATION;
    a->landStrength = 0.6f + 0.4f * Clamp(in.speed / 6.0f, 0.0f, 1.0f);
    a->ikRamp = 0.0f; // ease the slope IK back in while the capsule settles
  }
  if (in.airborne) a->ikRamp = 0.0f;
  else a->ikRamp = fminf(a->ikRamp + dt / 0.25f, 1.0f);
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
  if (landDip > 0.0001f && a->bonePelvis >= 0 &&
      a->bonePelvis < a->boneCount)
    a->blendPose[a->bonePelvis].translation.y -= landDip;

  // --- procedural foot IK (leg bending/extension on slopes and steps) ------
  if (a->boneThighL >= 0 && a->boneCalfL >= 0 && a->boneFootL >= 0 &&
      a->boneThighR >= 0 && a->boneCalfR >= 0 && a->boneFootR >= 0) {
    const float thighGain = 1.5f;
    const float calfGain = 3.0f;
    const float footGain = 1.5f;

    // Slope IK is weighted by the post-touchdown ramp (raycasts right at
    // landing are noisy: a probe off a ledge reads metres of drop and the
    // legs snap). The landing knee-bend is NOT ramped — it IS the landing.
    float landBend = landDip * LAND_KNEE_RATIO;
    float dl = in.leftFootDelta * a->ikRamp + landBend;
    float dr = in.rightFootDelta * a->ikRamp + landBend;
    float pitch = in.groundPitch * a->ikRamp;

    if (fabsf(dl) > 0.005f) {
      Quaternion qThigh = QuaternionFromEuler(-dl * thighGain, 0.0f, 0.0f);
      a->blendPose[a->boneThighL].rotation =
          QuaternionMultiply(a->blendPose[a->boneThighL].rotation, qThigh);
      Quaternion qCalf = QuaternionFromEuler(dl * calfGain, 0.0f, 0.0f);
      a->blendPose[a->boneCalfL].rotation =
          QuaternionMultiply(a->blendPose[a->boneCalfL].rotation, qCalf);
      Quaternion qFoot = QuaternionFromEuler(-dl * footGain, 0.0f, 0.0f);
      a->blendPose[a->boneFootL].rotation =
          QuaternionMultiply(a->blendPose[a->boneFootL].rotation, qFoot);
    }
    if (fabsf(pitch) > 0.01f) {
      // Lay both feet onto the slope along the facing direction.
      Quaternion qSlope = QuaternionFromEuler(pitch, 0.0f, 0.0f);
      a->blendPose[a->boneFootL].rotation =
          QuaternionMultiply(a->blendPose[a->boneFootL].rotation, qSlope);
      a->blendPose[a->boneFootR].rotation =
          QuaternionMultiply(a->blendPose[a->boneFootR].rotation, qSlope);
    }
    if (fabsf(dr) > 0.005f) {
      Quaternion qThigh = QuaternionFromEuler(-dr * thighGain, 0.0f, 0.0f);
      a->blendPose[a->boneThighR].rotation =
          QuaternionMultiply(a->blendPose[a->boneThighR].rotation, qThigh);
      Quaternion qCalf = QuaternionFromEuler(dr * calfGain, 0.0f, 0.0f);
      a->blendPose[a->boneCalfR].rotation =
          QuaternionMultiply(a->blendPose[a->boneCalfR].rotation, qCalf);
      Quaternion qFoot = QuaternionFromEuler(-dr * footGain, 0.0f, 0.0f);
      a->blendPose[a->boneFootR].rotation =
          QuaternionMultiply(a->blendPose[a->boneFootR].rotation, qFoot);
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
