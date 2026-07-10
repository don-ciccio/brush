/*******************************************************************************************
 *   b_anim.h - Skeletal character animator
 *
 *   Owns a set of named clips loaded from one glTF/GLB and produces a skinned
 *   pose each frame from a small gameplay-input struct (never a Player*), so
 *   any character can use it.
 *
 *   Structure (ported from the donor codebase's animator):
 *     - LOCOMOTION: 1-D blend over speed with a shared gait phase
 *       (idle -> walk -> jog -> sprint), feet phase-synced across the blend.
 *     - CROUCH: same 1-D idea over the crouch set (idle <-> forward).
 *     - Jumps: START (one-shot) -> LOOP (airborne hold) -> LAND (one-shot;
 *       skipped when landing at speed — rolls straight back into the gait).
 *     - ROLL: one-shot fired by BrushAnimatorTriggerRoll, exits to
 *       locomotion/crouch.
 *     - State changes cross-fade from a snapshot of the previous pose.
 *
 *   PROCEDURAL LAYER (applied on top of the blended pose):
 *     - Landing absorption: a short squat-and-recover fired on the
 *       airborne->grounded edge; the pelvis dips and both knees bend while
 *       the feet stay planted — the weight a plain cross-fade lacks.
 *     - Foot IK: per-foot height deltas (from game raycasts) bend each leg
 *       on slopes/steps so feet rest on the ground while the body stays
 *       UPRIGHT. Pair it with a pelvis drop on the model transform (see
 *       BrushAnimInput) — the body lowers to the downhill foot and the
 *       uphill knee bends.
 *
 *   Legs are found BY BONE NAME at init (pelvis, thigh_l/r, calf_l/r,
 *   foot_l/r — the UE naming the Quaternius rigs use). If a name is missing
 *   the procedural layer is skipped; remap via the bone* fields after init
 *   for other skeletons.
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
  BRUSH_CLIP_CROUCH_IDLE,
  BRUSH_CLIP_CROUCH_WALK,
  BRUSH_CLIP_ROLL,
  BRUSH_CLIP_COUNT
} BrushClip;

typedef enum {
  BRUSH_ANIM_LOCOMOTION = 0,
  BRUSH_ANIM_CROUCH,
  BRUSH_ANIM_JUMP_START,
  BRUSH_ANIM_JUMP_LOOP,
  BRUSH_ANIM_JUMP_LAND,
  BRUSH_ANIM_ROLL,
} BrushAnimState;

// Per-frame gameplay inputs. Keeping the animator driven by this struct is
// what makes it reusable across characters.
typedef struct BrushAnimInput {
  float speed;   // horizontal speed, m/s (drives the locomotion blend)
  bool airborne; // true while off the ground. Debounce this (e.g. only after
                 // ~60ms of continuous air time): physics ground states
                 // flicker during touchdown, and a raw flag re-triggers the
                 // jump state every flicker (visible landing stutter).
  bool crouched; // hold-to-crouch gameplay state

  // --- Foot IK (standard pipeline: rays from the ANIMATED feet, pelvis
  // lowers to the lowest foot, analytic two-bone IK per leg, ankle aimed to
  // the ground normal). Set groundFn to enable; the animator computes the
  // animated foot world positions itself from worldPos/yawRad.
  //
  // groundFn: return true and fill *outHeight (world Y) — and *outNormal if
  // non-NULL — for the ground under `probe` (a world position; query
  // downward from ~1m above it). NULL disables foot IK.
  bool (*groundFn)(void *user, Vector3 probe, float *outHeight,
                   Vector3 *outNormal);
  void *groundUser;
  Vector3 worldPos; // character feet world position (the model origin)
  float yawRad;     // model yaw applied at draw time
  float ikWeight;   // shared IK weight: ramp 0->1 over ~0.25s after
                    // touchdown, 0 while airborne (landing rays are noisy)
} BrushAnimInput;

typedef struct BrushAnimator {
  Model *model; // target skinned model (not owned)
  int boneCount;

  ModelAnimation *anims; // all clips from the source file (owned)
  int animCount;
  int clipIndex[BRUSH_CLIP_COUNT]; // clip id -> index into anims, -1 if absent

  // Locomotion blend axis: speeds (m/s) at which each gait clip is "pure".
  float walkSpeed, jogSpeed, sprintSpeed;
  float crouchSpeed; // speed at which the crouch-walk clip is "pure"

  BrushAnimState state;
  float phase;       // normalized clip/gait phase [0,1)
  float speedSmooth; // eased speed parameter for the locomotion blend

  // Cross-fade from a frozen snapshot of the previous pose.
  float fadeT, fadeDur;

  // Landing absorption state
  bool prevAirborne;
  float landTimer;    // seconds remaining in the current dip (0 = idle)
  float landStrength; // 0..1 magnitude captured at impact
  bool landDebugPin;  // BRUSH_ANIM_LAND: hold the dip for screenshots

  // Foot IK state (smoothed per-foot terrain deltas) + output.
  float footDeltaL, footDeltaR;
  float pelvisOffset; // OUTPUT: add to the model's draw Y (<= 0). Includes
                      // the terrain pelvis drop and the landing dip.

  // Inertia leans (procedural: bank into acceleration and turns).
  float prevSpeedSmooth;
  float prevYawRad;
  float leanFwd, leanSide;

  // Leg bones resolved by name at init (-1 = not found, IK skipped).
  int boneSpine1, boneSpine2;
  int bonePelvis;
  int boneThighL, boneCalfL, boneFootL;
  int boneThighR, boneCalfR, boneFootR;
  int boneBallL, boneBallR; // toe joints (optional): second ground probe

  // Scratch pose buffers (boneCount entries each).
  Transform *blendPose; // final pose applied to the model
  Transform *poseA, *poseB;
  Transform *fadePose; // snapshot taken at each state change

  ModelAnimation blendAnim; // 1-frame wrapper so raylib can skin blendPose
} BrushAnimator;

// Load every animation in `animFile` (usually the same file the model came
// from) and bind clip ids by name. `clipNames` maps BRUSH_CLIP_* -> clip name
// in the file; pass NULL for the built-in Quaternius UAL names.
// Returns false if the file has no usable clips.
bool BrushAnimatorInit(BrushAnimator *a, Model *model, const char *animFile,
                       const char *const clipNames[BRUSH_CLIP_COUNT]);

// Advance the state machine, apply the procedural layer, and pose the model
// (CPU skinning via UpdateModelAnimation). Call once per rendered frame.
void BrushAnimatorUpdate(BrushAnimator *a, BrushAnimInput in, float dt);

// Fire the roll one-shot (no-op if already rolling or the clip is missing).
void BrushAnimatorTriggerRoll(BrushAnimator *a);

BrushAnimState BrushAnimatorState(const BrushAnimator *a);

void BrushAnimatorUnload(BrushAnimator *a);

#endif // B_ANIM_H
