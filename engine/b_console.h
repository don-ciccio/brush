/*******************************************************************************************
 *   b_console.h - In-game debug console (F1)
 *
 *   Captures every TraceLog message into a ring buffer and draws the tail as
 *   an overlay, plus live frame stats and the active render layer view.
 *   Read-only in v0 (no command input yet).
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_CONSOLE_H
#define B_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

void BrushConsoleInit(void); // installs the TraceLog callback
void BrushConsoleToggle(void);
bool BrushConsoleIsOpen(void);
void BrushConsoleDraw(void); // no-op while closed

#ifdef __cplusplus
}
#endif

#endif // B_CONSOLE_H
