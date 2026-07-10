/*******************************************************************************************
 *   brush.h - Public umbrella header for the brush engine
 *
 *   brush is a small C11 + raylib engine for third-person open-world games,
 *   extracted from a working game codebase. Its renderer is organized as
 *   explicit ordered layers (shadow, opaque color, sky, transparent, volume)
 *   so every stage of the image can be built, inspected, and replaced
 *   independently.
 *
 *   A game links libbrush.a and includes only this header. The engine never
 *   includes game code.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef BRUSH_H
#define BRUSH_H

#include "b_anim.h"
#include "b_app.h"
#include "b_camera.h"
#include "b_console.h"
#include "b_input.h"
#include "b_post.h"
#include "b_render.h"
#include "b_sky.h"

#endif // BRUSH_H
