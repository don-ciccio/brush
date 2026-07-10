/*******************************************************************************************
 *   b_anim.c - Skeletal character animator
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_anim.h"

#include <raymath.h>
#include <stddef.h>
#include <string.h>

// How fast the eased speed parameter chases the real speed (~0.25 s settle).
#define LOCO_BLEND_RATE 9.0f
// Cross-fade durations (seconds).
#define FADE_TO_JUMP 0.08f
#define FADE_TO_LAND 0.06f
#define FADE_TO_LOCO 0.15f
// Leave the land state after this fraction of the land clip (its tail is a
// slow settle that reads better blended into locomotion).
#define LAND_EXIT_PHASE 0.55f
// Above this speed (m/s) a landing skips the plant-the-feet land clip and
// cross-fades straight into locomotion — otherwise the character moonwalks
// while the clip holds its feet still.
#define LAND_RUN_THROUGH_SPEED 1.5f

static const char *const UAL_CLIP_NAMES[BRUSH_CLIP_COUNT] = {
    "Idle_Loop",  "Walk_Loop",  "Jog_Fwd_Loop", "Sprint_Loop",
    "Jump_Start", "Jump_Loop",  "Jump_Land",
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

// --- public API -------------------------------------------------------------

bool BrushAnimatorInit(BrushAnimator *a, Model *model, const char *animFile,
                       const char *const clipNames[BRUSH_CLIP_COUNT]) {
  *a = (BrushAnimator){0};
  a->model = model;
  a->boneCount = model->boneCount;
  a->walkSpeed = 2.0f;
  a->jogSpeed = 4.5f;
  a->sprintSpeed = 7.0f;
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
      TraceLog(LOG_INFO, "BRUSH anim: %-12s -> '%s' (%d frames, %d bones)",
               clipNames[c], a->anims[a->clipIndex[c]].name,
               a->anims[a->clipIndex[c]].frameCount,
               a->anims[a->clipIndex[c]].boneCount);
  }
  if (a->clipIndex[BRUSH_CLIP_IDLE] < 0) return false;

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

// Evaluate the 1-D locomotion blend at the shared gait phase and return the
// cycle rate of the blended gait (so the phase advances at a matched cadence).
static float EvalLocomotion(BrushAnimator *a, Transform *out) {
  // Blend axis: (speed, clip) anchor points, in ascending speed order.
  struct BlendPt { float speed; BrushClip clip; } pts[4];
  int n = 0;
  pts[n++] = (struct BlendPt){0.0f, BRUSH_CLIP_IDLE};
  if (a->clipIndex[BRUSH_CLIP_WALK] >= 0)
    pts[n++] = (struct BlendPt){a->walkSpeed, BRUSH_CLIP_WALK};
  if (a->clipIndex[BRUSH_CLIP_JOG] >= 0)
    pts[n++] = (struct BlendPt){a->jogSpeed, BRUSH_CLIP_JOG};
  if (a->clipIndex[BRUSH_CLIP_SPRINT] >= 0)
    pts[n++] = (struct BlendPt){a->sprintSpeed, BRUSH_CLIP_SPRINT};

  float s = a->speedSmooth;
  int hi = 1;
  while (hi < n - 1 && s > pts[hi].speed) hi++;
  if (n == 1) hi = 0;
  int lo = (hi > 0) ? hi - 1 : 0;
  float span = pts[hi].speed - pts[lo].speed;
  float w = (span > 0.001f) ? (s - pts[lo].speed) / span : 0.0f;
  if (w < 0.0f) w = 0.0f;
  if (w > 1.0f) w = 1.0f;

  const ModelAnimation *ca = Clip(a, pts[lo].clip);
  const ModelAnimation *cb = Clip(a, pts[hi].clip);
  SampleClip(ca, a->phase, a->poseA, a->boneCount, true);
  if (cb != ca && w > 0.0f) {
    SampleClip(cb, a->phase, a->poseB, a->boneCount, true);
    BlendBonePoses(out, a->poseA, a->poseB, a->boneCount, w);
  } else {
    memcpy(out, a->poseA, sizeof(Transform) * (size_t)a->boneCount);
  }
  return Lerp(ClipRate(ca), ClipRate(cb), w);
}

void BrushAnimatorUpdate(BrushAnimator *a, BrushAnimInput in, float dt) {
  if (a->blendPose == NULL) return;

  // Eased speed parameter so gait changes settle instead of snapping.
  a->speedSmooth += (in.speed - a->speedSmooth) *
                    fminf(LOCO_BLEND_RATE * dt, 1.0f);

  // --- transitions ---
  // Landing at speed rolls straight back into the gait; the land clip is
  // for (near-)stationary touchdowns only.
  bool landToLoco = (in.speed > LAND_RUN_THROUGH_SPEED);
  switch (a->state) {
  case BRUSH_ANIM_LOCOMOTION:
    if (in.airborne) EnterState(a, BRUSH_ANIM_JUMP_START, FADE_TO_JUMP);
    break;
  case BRUSH_ANIM_JUMP_START:
    if (!in.airborne)
      EnterState(a, landToLoco ? BRUSH_ANIM_LOCOMOTION : BRUSH_ANIM_JUMP_LAND,
                 landToLoco ? 0.12f : FADE_TO_LAND);
    else if (a->phase >= 1.0f) EnterState(a, BRUSH_ANIM_JUMP_LOOP, 0.10f);
    break;
  case BRUSH_ANIM_JUMP_LOOP:
    if (!in.airborne)
      EnterState(a, landToLoco ? BRUSH_ANIM_LOCOMOTION : BRUSH_ANIM_JUMP_LAND,
                 landToLoco ? 0.12f : FADE_TO_LAND);
    break;
  case BRUSH_ANIM_JUMP_LAND:
    if (in.airborne) EnterState(a, BRUSH_ANIM_JUMP_START, FADE_TO_JUMP);
    else if (a->phase >= LAND_EXIT_PHASE)
      EnterState(a, BRUSH_ANIM_LOCOMOTION, FADE_TO_LOCO);
    break;
  }

  // --- evaluate the current state's pose into blendPose ---
  float rate = 0.0f;
  const ModelAnimation *clip = NULL;
  switch (a->state) {
  case BRUSH_ANIM_LOCOMOTION:
    rate = EvalLocomotion(a, a->blendPose);
    break;
  case BRUSH_ANIM_JUMP_START:
    clip = Clip(a, BRUSH_CLIP_JUMP_START);
    break;
  case BRUSH_ANIM_JUMP_LOOP:
    clip = Clip(a, BRUSH_CLIP_JUMP_LOOP);
    break;
  case BRUSH_ANIM_JUMP_LAND:
    clip = Clip(a, BRUSH_CLIP_JUMP_LAND);
    break;
  }
  if (clip == NULL && a->state != BRUSH_ANIM_LOCOMOTION) {
    // Missing jump clip: fall back to locomotion so the character never
    // freezes.
    rate = EvalLocomotion(a, a->blendPose);
  } else if (clip != NULL) {
    bool loop = (a->state == BRUSH_ANIM_JUMP_LOOP);
    SampleClip(clip, a->phase, a->blendPose, a->boneCount, loop);
    rate = ClipRate(clip);
  }

  // Advance the phase; loops wrap, one-shots clamp just past the end so
  // "finished" transitions fire.
  a->phase += rate * dt;
  if (a->state == BRUSH_ANIM_LOCOMOTION || a->state == BRUSH_ANIM_JUMP_LOOP) {
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
