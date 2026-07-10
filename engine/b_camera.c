/*******************************************************************************************
 *   b_camera.c - Third-person orbit camera with hybrid auto-follow
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_camera.h"
#include "b_input.h"

#include <math.h>
#include <raymath.h>
#include <stddef.h>

void BrushOrbitCamInit(BrushOrbitCam *c, Vector3 focus) {
  c->cam.position = Vector3Add(focus, (Vector3){0.0f, 3.0f, 6.0f});
  c->cam.target = Vector3Add(focus, (Vector3){0.0f, 1.0f, 0.0f});
  c->cam.up = (Vector3){0.0f, 1.0f, 0.0f};
  c->cam.fovy = 45.0f;
  c->cam.projection = CAMERA_PERSPECTIVE;

  c->angle = 0.0f;
  c->height = 2.5f;
  c->distance = 6.0f;
  c->minDistance = 2.5f;
  c->maxDistance = 15.0f;
  c->minHeight = 1.0f;
  c->maxHeight = 8.0f;
  c->followSmooth = 5.0f;

  c->idleTimer = 0.0f;
  c->autoFollowDelay = 2.5f;
  c->autoFollowSpeed = 0.8f;

  c->groundHeightFn = NULL;
  c->groundHeightUser = NULL;
  c->groundClearance = 0.5f;
}

void BrushOrbitCamUpdate(BrushOrbitCam *c, Vector3 focus, Vector3 focusVel,
                         float dt) {
  // --- Manual orbit input ---
  float lookX = BrushInputAxis(BRUSH_AXIS_LOOK_X);
  float lookY = BrushInputAxis(BRUSH_AXIS_LOOK_Y);
  bool manual = (fabsf(lookX) > 0.0001f) || (fabsf(lookY) > 0.0001f);

  c->angle += lookX;
  c->height = Clamp(c->height + lookY, c->minHeight, c->maxHeight);

  float wheel = GetMouseWheelMove();
  if (wheel != 0)
    c->distance = Clamp(c->distance - wheel * 0.8f, c->minDistance,
                        c->maxDistance);

  // --- Hybrid auto-follow: drift behind the movement heading when idle ---
  if (manual)
    c->idleTimer = 0.0f;
  else
    c->idleTimer += dt;

  float horizSpeed = sqrtf(focusVel.x * focusVel.x + focusVel.z * focusVel.z);
  if (!manual && c->idleTimer > c->autoFollowDelay && horizSpeed > 0.3f) {
    // Boom offset is (sin(angle)*d, h, cos(angle)*d), so "behind" the
    // velocity (vx, vz) means angle = atan2(vx, vz) + PI.
    float targetAngle = atan2f(focusVel.x, focusVel.z) + PI;
    float diff = targetAngle - c->angle;
    while (diff > PI) diff -= 2.0f * PI;
    while (diff < -PI) diff += 2.0f * PI;
    // Dead zone (~15 deg) avoids jittery micro-corrections; ease in over the
    // first second so the drift doesn't start with a jerk.
    if (fabsf(diff) > 0.26f) {
      float easeIn = Clamp((c->idleTimer - c->autoFollowDelay) / 1.0f, 0, 1);
      float maxStep = c->autoFollowSpeed * easeIn * dt;
      c->angle += Clamp(diff, -maxStep, maxStep);
    }
  }

  // --- Boom position + ground clamp ---
  Vector3 offset = {sinf(c->angle) * c->distance, c->height,
                    cosf(c->angle) * c->distance};
  Vector3 wantPos = Vector3Add(focus, offset);

  float groundY = c->groundHeightFn
                      ? c->groundHeightFn(wantPos.x, wantPos.z,
                                          c->groundHeightUser)
                      : 0.0f;
  if (wantPos.y < groundY + c->groundClearance)
    wantPos.y = groundY + c->groundClearance;

  Vector3 wantTarget = {focus.x, focus.y + 1.0f, focus.z};

  float s = Clamp(c->followSmooth * dt, 0.0f, 1.0f);
  c->cam.position = Vector3Lerp(c->cam.position, wantPos, s);
  c->cam.target = Vector3Lerp(c->cam.target, wantTarget, s);
}
