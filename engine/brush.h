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

#ifdef __cplusplus
extern "C" {
#endif

#include "b_anim.h"
#include "b_app.h"
#include "b_assets.h"
#include "b_camera.h"
#include "b_character.h"
#include "b_console.h"
#include "b_foliage.h"
#include "b_input.h"
#include "b_meshlod.h"
#include "b_physics.h"
#include "b_post.h"
#include "b_project.h"
#include "b_render.h"
#include "b_scene.h"
#include "b_shadow.h"
#include "b_sky.h"
#include "b_tod.h"
#include "b_world.h"

#ifdef __cplusplus
}
#endif

#endif // BRUSH_H
