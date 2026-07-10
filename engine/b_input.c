/*******************************************************************************************
 *   b_input.c - Action-based input (keyboard + gamepad)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_input.h"

#include <math.h>
#include <raylib.h>

#define PAD 0
#define STICK_DEADZONE 0.15f
#define MOUSE_ORBIT_SENS 0.005f
#define STICK_ORBIT_SENS 0.05f

static float ApplyDeadzone(float v) {
  return (fabsf(v) > STICK_DEADZONE) ? v : 0.0f;
}

float BrushInputAxis(BrushAxis axis) {
  float v = 0.0f;
  switch (axis) {
  case BRUSH_AXIS_MOVE_X:
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) v -= 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) v += 1.0f;
    if (IsGamepadAvailable(PAD))
      v += ApplyDeadzone(GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_LEFT_X));
    break;
  case BRUSH_AXIS_MOVE_Y:
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) v += 1.0f;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) v -= 1.0f;
    if (IsGamepadAvailable(PAD))
      v -= ApplyDeadzone(GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_LEFT_Y));
    break;
  case BRUSH_AXIS_LOOK_X:
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
      v -= GetMouseDelta().x * MOUSE_ORBIT_SENS;
    if (IsKeyDown(KEY_Q)) v += 2.0f * GetFrameTime();
    if (IsKeyDown(KEY_E)) v -= 2.0f * GetFrameTime();
    if (IsGamepadAvailable(PAD))
      v -= ApplyDeadzone(GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_RIGHT_X)) *
           STICK_ORBIT_SENS;
    break;
  case BRUSH_AXIS_LOOK_Y:
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
      v -= GetMouseDelta().y * MOUSE_ORBIT_SENS * 4.0f;
    if (IsGamepadAvailable(PAD))
      v -= ApplyDeadzone(GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_RIGHT_Y)) *
           STICK_ORBIT_SENS;
    break;
  default:
    break;
  }
  if (v > 1.0f && (axis == BRUSH_AXIS_MOVE_X || axis == BRUSH_AXIS_MOVE_Y))
    v = 1.0f;
  if (v < -1.0f && (axis == BRUSH_AXIS_MOVE_X || axis == BRUSH_AXIS_MOVE_Y))
    v = -1.0f;
  return v;
}

bool BrushInputDown(BrushButton btn) {
  bool pad = IsGamepadAvailable(PAD);
  switch (btn) {
  case BRUSH_BTN_JUMP:
    return IsKeyDown(KEY_SPACE) ||
           (pad && IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
  case BRUSH_BTN_SPRINT:
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ||
           (pad && IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_FACE_LEFT));
  case BRUSH_BTN_CROUCH:
    return IsKeyDown(KEY_LEFT_CONTROL) ||
           (pad && IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_THUMB));
  case BRUSH_BTN_ROLL:
    return IsKeyDown(KEY_R) ||
           (pad && IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
  case BRUSH_BTN_MENU:
    return IsKeyDown(KEY_TAB) ||
           (pad && IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_MIDDLE_RIGHT));
  case BRUSH_BTN_ACCEPT:
    return IsKeyDown(KEY_ENTER) ||
           (pad && IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
  case BRUSH_BTN_UP:
    return IsKeyDown(KEY_UP) ||
           (pad && IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_UP));
  case BRUSH_BTN_DOWN:
    return IsKeyDown(KEY_DOWN) ||
           (pad && IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_DOWN));
  default:
    return false;
  }
}

bool BrushInputPressed(BrushButton btn) {
  bool pad = IsGamepadAvailable(PAD);
  switch (btn) {
  case BRUSH_BTN_JUMP:
    return IsKeyPressed(KEY_SPACE) ||
           (pad && IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
  case BRUSH_BTN_SPRINT:
    return IsKeyPressed(KEY_LEFT_SHIFT) ||
           (pad && IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_RIGHT_FACE_LEFT));
  case BRUSH_BTN_CROUCH:
    return IsKeyPressed(KEY_LEFT_CONTROL) ||
           (pad && IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_RIGHT_THUMB));
  case BRUSH_BTN_ROLL:
    return IsKeyPressed(KEY_R) ||
           (pad && IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
  case BRUSH_BTN_MENU:
    return IsKeyPressed(KEY_TAB) ||
           (pad && IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_MIDDLE_RIGHT));
  case BRUSH_BTN_ACCEPT:
    return IsKeyPressed(KEY_ENTER) ||
           (pad && IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
  case BRUSH_BTN_UP:
    return IsKeyPressed(KEY_UP) ||
           (pad && IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_LEFT_FACE_UP));
  case BRUSH_BTN_DOWN:
    return IsKeyPressed(KEY_DOWN) ||
           (pad && IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_LEFT_FACE_DOWN));
  default:
    return false;
  }
}
