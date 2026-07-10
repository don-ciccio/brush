/*******************************************************************************************
 *   b_tod.h - Day/night cycle: game clock + celestial positioning
 *
 *   Single authority on game time. The clock (0..24 h) drives sun and moon
 *   directions from real solar geometry (hour angle + seasonal declination +
 *   latitude), and a set of elevation-keyed lookup tables turns the sun's
 *   elevation into light color, ambient fill, fog color, and exposure.
 *
 *   The LUTs define the LOOK and are per-instance data: the engine ships a
 *   neutral default palette, and a game can point any table at its own
 *   keyframes (sorted by ascending elevation) to restyle dawn/noon/night
 *   without touching engine code.
 *
 *   Feed the results to the renderer with BrushRenderApplyTimeOfDay(), or
 *   read the individual queries and wire them yourself.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_TOD_H
#define B_TOD_H

#include <raylib.h>

// Elevation-keyed keyframes (elevation = sun direction Y, -1..1).
typedef struct BrushTodColorKey {
  float elevation;
  Vector3 value;
} BrushTodColorKey;

typedef struct BrushTodFloatKey {
  float elevation;
  float value;
} BrushTodFloatKey;

typedef struct BrushTimeOfDay {
  float timeHours;    // 0..24 master clock (12 = noon, 0/24 = midnight)
  float dayLengthSec; // real seconds per in-game day (1440 = 24 min)
  float timeScale;    // time progression multiplier; 0 = frozen
  float latitudeDeg;  // celestial latitude (tilts the sun path)
  float dayOfYear;    // 1..365, drives seasonal declination

  // Look palette: elevation-keyed tables, smoothstep-interpolated. Init
  // points these at the engine defaults; a game may swap in its own arrays
  // (they are read live, not copied — keep them alive).
  const BrushTodColorKey *sunColorLUT;     int sunColorCount;
  const BrushTodColorKey *ambientLUT;      int ambientCount;
  const BrushTodFloatKey *ambientLevelLUT; int ambientLevelCount;
  const BrushTodColorKey *fogColorLUT;     int fogColorCount;
  const BrushTodFloatKey *exposureLUT;     int exposureCount;
} BrushTimeOfDay;

// Defaults: 24-minute day starting mid-morning. BRUSH_TIME=<hours> overrides
// the start time (6.3 = dawn, 12 = noon, 0 = midnight) for reproducible
// captures; BRUSH_DAY_LENGTH=<seconds> overrides the day length.
void BrushTodInit(BrushTimeOfDay *tod);

// Advance the clock (wraps at 24 h).
void BrushTodUpdate(BrushTimeOfDay *tod, float dt);

// Directions point TOWARD the body (normalized, world space, Y up).
Vector3 BrushTodSunDir(const BrushTimeOfDay *tod);
Vector3 BrushTodMoonDir(const BrushTimeOfDay *tod);

float BrushTodSunElevation(const BrushTimeOfDay *tod); // sun dir Y, -1..1

// Look queries (LUT-driven, linear-light values).
Vector3 BrushTodSunColor(const BrushTimeOfDay *tod);
Vector3 BrushTodMoonColor(const BrushTimeOfDay *tod); // black below horizon
Vector3 BrushTodAmbientColor(const BrushTimeOfDay *tod); // color * level
Vector3 BrushTodFogColor(const BrushTimeOfDay *tod);
float BrushTodExposure(const BrushTimeOfDay *tod);

#endif // B_TOD_H
