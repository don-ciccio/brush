/*******************************************************************************************
 *   b_biome.h - Biome field types (docs/biome-system-plan.md)
 *
 *   A biome is a named bundle of look (terrain palette, foliage set, grass
 *   colour, mood). The BIOME FIELD assigns, per world (x,z), the two dominant
 *   biome IDs + a blend factor. It's resolved per-chunk on the worker like the
 *   splat/foliage-density maps and read back through a new `biomeAt` sampler.
 *
 *   Phase 0 (this header + b_world backing) ships the field, the per-chunk map,
 *   the sampler and an F2 debug view — no gameplay/visual change until foliage
 *   (Phase 1) and terrain (Phase 2) consume it.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_BIOME_H
#define B_BIOME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define BRUSH_MAX_BIOMES 16      // IDs 0..15 (decision #4: 16-biome ceiling)
#define BRUSH_BIOME_WHITTAKER 8  // NxN (temperature x moisture) climate lookup

// Per-point biome resolution: the two dominant biomes + how much the second
// mixes in. blend 0 => all id0; blend 1 => all id1. (Decision #2: 2 biomes/texel.)
typedef struct BrushBiomeSample {
  unsigned char id0, id1;
  float blend;
} BrushBiomeSample;

// The climate field + Whittaker lookup that generate the procedural base of the
// biome field. Lives on the world (set from the scene via BrushWorldSetBiomeClimate).
// biomeCount 0 => biomes OFF: a single implicit biome 0 everywhere, and biomeAt
// returns {0,0,0} (a v<=3 / biome-less scene behaves exactly as before).
typedef struct BrushBiomeClimate {
  int biomeCount;              // number of biomes defined (0 = off)
  unsigned int seed;
  float tempScale, moistScale; // world metres per noise period (larger = broader regions)
  float lapse;                 // temperature drop per metre of height above seaLevel
  float seaLevel;              // reference height for the lapse
  float warp;                  // domain-warp amount (metres) so borders meander
  float blendRadius;           // scattered-blend radius (metres); 0 => hard borders
  // [tempCell * N + moistCell] -> biomeId, N = BRUSH_BIOME_WHITTAKER.
  unsigned char whittaker[BRUSH_BIOME_WHITTAKER * BRUSH_BIOME_WHITTAKER];
} BrushBiomeClimate;

#ifdef __cplusplus
}
#endif

#endif // B_BIOME_H
