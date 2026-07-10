/*******************************************************************************************
 *   b_input.h - Action-based input (keyboard + gamepad)
 *
 *   Gameplay code asks for actions and axes, never raw keys, so bindings can
 *   become data-driven (config file, rebinding UI) without touching gameplay.
 *   v0 ships fixed default bindings:
 *
 *     move        WASD / arrows / gamepad left stick
 *     look        RMB-drag mouse / Q,E / gamepad right stick
 *     jump        Space / pad A          sprint  Shift / pad X
 *     menu        Tab / pad Start        accept  Enter / pad A
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_INPUT_H
#define B_INPUT_H

#include <stdbool.h>

typedef enum {
  BRUSH_AXIS_MOVE_X = 0, // strafe: -1 left .. +1 right
  BRUSH_AXIS_MOVE_Y,     // forward: +1 forward .. -1 back
  BRUSH_AXIS_LOOK_X,     // orbit yaw input (already sensitivity-scaled)
  BRUSH_AXIS_LOOK_Y,     // orbit height input
  BRUSH_AXIS_COUNT
} BrushAxis;

typedef enum {
  BRUSH_BTN_JUMP = 0,
  BRUSH_BTN_SPRINT,
  BRUSH_BTN_MENU,
  BRUSH_BTN_ACCEPT,
  BRUSH_BTN_UP,   // menu navigation
  BRUSH_BTN_DOWN, // menu navigation
  BRUSH_BTN_COUNT
} BrushButton;

float BrushInputAxis(BrushAxis axis);
bool BrushInputDown(BrushButton btn);
bool BrushInputPressed(BrushButton btn);

#endif // B_INPUT_H
