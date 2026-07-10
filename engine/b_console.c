/*******************************************************************************************
 *   b_console.c - In-game debug console (F1)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_console.h"
#include "b_render.h"

#include <raylib.h>
#include <stdio.h>
#include <string.h>

#define CONSOLE_LINES 128
#define CONSOLE_LINE_LEN 192

typedef struct BrushConsoleState {
  char lines[CONSOLE_LINES][CONSOLE_LINE_LEN];
  int head;  // next slot to write
  int count; // total stored (saturates at CONSOLE_LINES)
  bool open;
} BrushConsoleState;

static BrushConsoleState g_con = {0};

static void ConsoleLogCallback(int logLevel, const char *text, va_list args) {
  char *line = g_con.lines[g_con.head];
  const char *tag = "";
  switch (logLevel) {
  case LOG_WARNING: tag = "[WARN] "; break;
  case LOG_ERROR: tag = "[ERROR] "; break;
  case LOG_DEBUG: tag = "[DEBUG] "; break;
  default: break;
  }
  int off = snprintf(line, CONSOLE_LINE_LEN, "%s", tag);
  if (off < 0) off = 0;
  vsnprintf(line + off, (size_t)(CONSOLE_LINE_LEN - off), text, args);
  g_con.head = (g_con.head + 1) % CONSOLE_LINES;
  if (g_con.count < CONSOLE_LINES) g_con.count++;

  // Mirror to stdout so terminal logs keep working.
  printf("%s\n", line);
}

void BrushConsoleInit(void) { SetTraceLogCallback(ConsoleLogCallback); }

void BrushConsoleToggle(void) { g_con.open = !g_con.open; }

bool BrushConsoleIsOpen(void) { return g_con.open; }

void BrushConsoleDraw(void) {
  if (!g_con.open) return;

  int w = GetScreenWidth();
  int h = GetScreenHeight() / 2;
  DrawRectangle(0, 0, w, h, (Color){10, 12, 16, 225});
  DrawRectangle(0, h - 2, w, 2, (Color){120, 190, 255, 255});

  DrawText(TextFormat("brush console  |  %d fps  %.2f ms  |  layer view: %s",
                      GetFPS(), GetFrameTime() * 1000.0f,
                      BrushRenderLayerViewName()),
           12, 10, 20, (Color){120, 190, 255, 255});

  const int lineH = 18;
  int visible = (h - 44) / lineH;
  if (visible > g_con.count) visible = g_con.count;
  for (int i = 0; i < visible; i++) {
    // i = 0 draws the oldest of the visible tail, ending at the newest line.
    int idx = (g_con.head - visible + i + CONSOLE_LINES * 2) % CONSOLE_LINES;
    Color c = (Color){200, 205, 215, 255};
    if (strstr(g_con.lines[idx], "[WARN]")) c = (Color){235, 200, 90, 255};
    if (strstr(g_con.lines[idx], "[ERROR]")) c = (Color){235, 100, 90, 255};
    DrawText(g_con.lines[idx], 12, 40 + i * lineH, 16, c);
  }
}
