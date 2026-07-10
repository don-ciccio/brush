/*******************************************************************************************
 *   b_camera.h - Third-person orbit camera with hybrid auto-follow
 *
 *   Free orbit (mouse RMB-drag / right stick / Q,E) + wheel zoom around a
 *   focus point, with a soft auto-follow: after `autoFollowDelay` seconds of
 *   no manual camera input while the focus is moving, the camera gently
 *   drifts behind the focus's world-space velocity (velocity, not facing,
 *   to avoid the camera->input->rotation feedback loop that oscillates).
 *
 *   The camera should follow the *interpolated render position* of the focus
 *   so it doesn't inherit fixed-timestep stutter.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_CAMERA_H
#define B_CAMERA_H

#include <raylib.h>
#include <stdbool.h>

typedef struct BrushOrbitCam {
  Camera3D cam;

  float angle;    // horizontal orbit angle (radians)
  float height;   // boom height above the focus
  float distance; // boom length
  float minDistance, maxDistance;
  float minHeight, maxHeight;
  float followSmooth; // lerp rate for position/target (per second)

  // Hybrid auto-follow state
  float idleTimer;
  float autoFollowDelay; // seconds of camera inactivity before drift
  float autoFollowSpeed; // radians/sec drift toward the movement heading

  // Optional ground clamp: keep the camera at least `groundClearance` above
  // groundHeightFn(x, z). NULL -> clamp against y = 0.
  float (*groundHeightFn)(float x, float z, void *user);
  void *groundHeightUser;
  float groundClearance;
} BrushOrbitCam;

void BrushOrbitCamInit(BrushOrbitCam *c, Vector3 focus);

// Per-frame: reads look input (b_input), applies zoom/orbit/auto-follow, and
// smooths toward the new boom position. `focusVel` is world-space velocity.
void BrushOrbitCamUpdate(BrushOrbitCam *c, Vector3 focus, Vector3 focusVel,
                         float dt);

#endif // B_CAMERA_H
