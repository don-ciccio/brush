/*******************************************************************************************
 *   b_anim.h - Skeletal character animator
 *
 *   Owns a set of named clips loaded from one glTF/GLB and produces a skinned
 *   pose each frame from a small gameplay-input struct (never a Player*), so
 *   any character can use it.
 *
 *   Structure (ported from the donor codebase's animator):
 *     - LOCOMOTION is a 1-D blend over speed with a shared gait phase:
 *       idle -> walk -> jog -> sprint, sampled at one normalized phase so feet
 *       stay in sync while the blend weight moves between clips.
 *     - Jumps are three phases: START (one-shot) -> LOOP (airborne hold) ->
 *       LAND (one-shot), then back to locomotion.
 *     - State changes cross-fade from a snapshot of the previous pose.
 *
 *   All clips must share the model's skeleton. Blending lerps translation and
 *   scale and slerps rotation per bone.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_ANIM_H
#define B_ANIM_H

#include <raylib.h>
#include <stdbool.h>

// raylib's glTF loader RESAMPLES animations at fixed 17 ms steps (rmodels.c
// GLTF_ANIMDELAY), so the authored frame rate is irrelevant — framePoses are
// always ~58.8/s. Play clips back at that rate, not the authored one.
#define BRUSH_ANIM_FPS (1000.0f / 17.0f)

typedef enum {
  BRUSH_CLIP_IDLE = 0,
  BRUSH_CLIP_WALK,
  BRUSH_CLIP_JOG,
  BRUSH_CLIP_SPRINT,
  BRUSH_CLIP_JUMP_START,
  BRUSH_CLIP_JUMP_LOOP,
  BRUSH_CLIP_JUMP_LAND,
  BRUSH_CLIP_COUNT
} BrushClip;

typedef enum {
  BRUSH_ANIM_LOCOMOTION = 0,
  BRUSH_ANIM_JUMP_START,
  BRUSH_ANIM_JUMP_LOOP,
  BRUSH_ANIM_JUMP_LAND,
} BrushAnimState;

// Per-frame gameplay inputs. Keeping the animator driven by this struct is
// what makes it reusable across characters.
typedef struct BrushAnimInput {
  float speed;   // horizontal speed, m/s (drives the locomotion blend)
  bool airborne; // true while off the ground
} BrushAnimInput;

typedef struct BrushAnimator {
  Model *model; // target skinned model (not owned)
  int boneCount;

  ModelAnimation *anims; // all clips from the source file (owned)
  int animCount;
  int clipIndex[BRUSH_CLIP_COUNT]; // clip id -> index into anims, -1 if absent

  // Locomotion blend axis: speeds (m/s) at which each gait clip is "pure".
  float walkSpeed, jogSpeed, sprintSpeed;

  BrushAnimState state;
  float phase;        // normalized clip/gait phase [0,1)
  float speedSmooth;  // eased speed parameter for the locomotion blend

  // Cross-fade from a frozen snapshot of the previous pose.
  float fadeT, fadeDur;

  // Scratch pose buffers (boneCount entries each).
  Transform *blendPose; // final pose applied to the model
  Transform *poseA, *poseB;
  Transform *fadePose;  // snapshot taken at each state change

  ModelAnimation blendAnim; // 1-frame wrapper so raylib can skin blendPose
} BrushAnimator;

// Load every animation in `animFile` (usually the same file the model came
// from) and bind clip ids by name. `clipNames` maps BRUSH_CLIP_* -> clip name
// in the file; pass NULL for the built-in Quaternius UAL names (Idle_Loop,
// Walk_Loop, Jog_Fwd_Loop, Sprint_Loop, Jump_Start, Jump_Loop, Jump_Land).
// Returns false if the file has no usable clips.
bool BrushAnimatorInit(BrushAnimator *a, Model *model, const char *animFile,
                       const char *const clipNames[BRUSH_CLIP_COUNT]);

// Advance the state machine and apply the blended pose to the model (CPU
// skinning via UpdateModelAnimation). Call once per rendered frame.
void BrushAnimatorUpdate(BrushAnimator *a, BrushAnimInput in, float dt);

void BrushAnimatorUnload(BrushAnimator *a);

#endif // B_ANIM_H
