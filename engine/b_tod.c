/*******************************************************************************************
 *   b_tod.c - Day/night cycle (see b_tod.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_tod.h"

#include <math.h>
#include <raymath.h>
#include <stdlib.h>

#define TOD_DEG2RAD(d) ((d) * (PI / 180.0f))

// --- Default look palette (neutral daylight cycle) --------------------------
// Elevation keys must ascend. Values are linear-light (the HDR pipeline tone
// maps once at the end).

static const BrushTodColorKey DEFAULT_SUN_COLOR[] = {
    {-0.20f, {0.00f, 0.00f, 0.00f}}, // deep night: no direct light
    {-0.05f, {0.00f, 0.00f, 0.00f}}, // late twilight
    {0.00f, {0.60f, 0.18f, 0.05f}},  // sunrise/sunset: deep crimson
    {0.08f, {0.88f, 0.53f, 0.25f}},  // golden hour
    {0.25f, {0.95f, 0.85f, 0.70f}},  // early day: warm white
    {0.60f, {1.00f, 0.95f, 0.90f}},  // midday: clean white
};

static const BrushTodColorKey DEFAULT_AMBIENT[] = {
    {-0.30f, {0.05f, 0.06f, 0.14f}}, // deep night: indigo fill
    {-0.05f, {0.12f, 0.10f, 0.25f}}, // late twilight: purple
    {0.00f, {0.38f, 0.36f, 0.52f}},  // sunset/sunrise: dusk violet
    {0.15f, {0.42f, 0.44f, 0.58f}},  // early day
    {0.60f, {0.50f, 0.55f, 0.68f}},  // noon: sky-blue fill
};

// Overall ambient level (multiplies the ambient color).
static const BrushTodFloatKey DEFAULT_AMBIENT_LEVEL[] = {
    {-0.30f, 0.25f}, {-0.05f, 0.40f}, {0.00f, 0.55f},
    {0.15f, 0.70f},  {0.60f, 0.85f},
};

static const BrushTodColorKey DEFAULT_FOG_COLOR[] = {
    {-0.30f, {0.02f, 0.02f, 0.05f}}, // night: near-black blue
    {-0.05f, {0.15f, 0.10f, 0.20f}}, // twilight: dark purple
    {0.00f, {0.80f, 0.35f, 0.15f}},  // sunrise/sunset: horizon glow
    {0.08f, {0.85f, 0.55f, 0.35f}},  // golden hour: peach
    {0.30f, {0.65f, 0.75f, 0.85f}},  // day: light sky blue
};

static const BrushTodFloatKey DEFAULT_EXPOSURE[] = {
    {-0.20f, 2.0f}, // night: eyes adapted to the dark
    {-0.05f, 1.5f}, // twilight
    {0.00f, 1.2f},  // sunrise/sunset
    {0.10f, 1.0f},  // golden hour
    {0.50f, 0.85f}, // day: hold back the blowout
};

#define LUT_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

// --- smoothstep-interpolated LUT lookups ------------------------------------

static float SegmentT(float e, float a, float b) {
  float t = (e - a) / (b - a);
  return t * t * (3.0f - 2.0f * t);
}

static Vector3 LookupColor(float e, const BrushTodColorKey *lut, int count) {
  if (lut == NULL || count <= 0) return (Vector3){0, 0, 0};
  if (e <= lut[0].elevation) return lut[0].value;
  if (e >= lut[count - 1].elevation) return lut[count - 1].value;
  for (int i = 0; i < count - 1; i++)
    if (e >= lut[i].elevation && e <= lut[i + 1].elevation)
      return Vector3Lerp(lut[i].value, lut[i + 1].value,
                         SegmentT(e, lut[i].elevation, lut[i + 1].elevation));
  return lut[count - 1].value;
}

static float LookupFloat(float e, const BrushTodFloatKey *lut, int count) {
  if (lut == NULL || count <= 0) return 0.0f;
  if (e <= lut[0].elevation) return lut[0].value;
  if (e >= lut[count - 1].elevation) return lut[count - 1].value;
  for (int i = 0; i < count - 1; i++)
    if (e >= lut[i].elevation && e <= lut[i + 1].elevation)
      return Lerp(lut[i].value, lut[i + 1].value,
                  SegmentT(e, lut[i].elevation, lut[i + 1].elevation));
  return lut[count - 1].value;
}

// --- public API --------------------------------------------------------------

void BrushTodInit(BrushTimeOfDay *tod) {
  *tod = (BrushTimeOfDay){0};
  tod->timeHours = 10.5f; // mid-morning
  const char *envTime = getenv("BRUSH_TIME");
  if (envTime != NULL) tod->timeHours = (float)atof(envTime);
  tod->dayLengthSec = 1440.0f; // 24 minutes per game day
  const char *envLen = getenv("BRUSH_DAY_LENGTH");
  if (envLen != NULL) tod->dayLengthSec = (float)atof(envLen);
  tod->timeScale = 1.0f;
  tod->latitudeDeg = 45.0f; // mid-latitudes
  tod->dayOfYear = 172.0f;  // summer solstice

  tod->sunColorLUT = DEFAULT_SUN_COLOR;
  tod->sunColorCount = LUT_COUNT(DEFAULT_SUN_COLOR);
  tod->ambientLUT = DEFAULT_AMBIENT;
  tod->ambientCount = LUT_COUNT(DEFAULT_AMBIENT);
  tod->ambientLevelLUT = DEFAULT_AMBIENT_LEVEL;
  tod->ambientLevelCount = LUT_COUNT(DEFAULT_AMBIENT_LEVEL);
  tod->fogColorLUT = DEFAULT_FOG_COLOR;
  tod->fogColorCount = LUT_COUNT(DEFAULT_FOG_COLOR);
  tod->exposureLUT = DEFAULT_EXPOSURE;
  tod->exposureCount = LUT_COUNT(DEFAULT_EXPOSURE);
}

void BrushTodUpdate(BrushTimeOfDay *tod, float dt) {
  if (tod->dayLengthSec <= 0.0f) return;
  tod->timeHours += (24.0f / tod->dayLengthSec) * dt * tod->timeScale;
  while (tod->timeHours >= 24.0f) tod->timeHours -= 24.0f;
  while (tod->timeHours < 0.0f) tod->timeHours += 24.0f;
}

// Shared celestial solver: hour angle + declination + latitude -> direction.
// x = east/west, y = up, z = south.
static Vector3 CelestialDir(float hourAngle, float declination,
                            float latitudeDeg) {
  float lat = TOD_DEG2RAD(latitudeDeg);
  Vector3 dir;
  dir.x = cosf(declination) * sinf(hourAngle);
  dir.y = cosf(declination) * cosf(hourAngle) * cosf(lat) +
          sinf(declination) * sinf(lat);
  dir.z = -cosf(declination) * cosf(hourAngle) * sinf(lat) +
          sinf(declination) * cosf(lat);
  return Vector3Normalize(dir);
}

static float SolarDeclination(float dayOfYear) {
  // Approximate: 23.45 deg * sin(2*pi/365 * (N - 80))
  return TOD_DEG2RAD(23.45f) * sinf((2.0f * PI / 365.0f) * (dayOfYear - 80.0f));
}

Vector3 BrushTodSunDir(const BrushTimeOfDay *tod) {
  float H = (tod->timeHours - 12.0f) * (2.0f * PI / 24.0f);
  return CelestialDir(H, SolarDeclination(tod->dayOfYear), tod->latitudeDeg);
}

Vector3 BrushTodMoonDir(const BrushTimeOfDay *tod) {
  // Opposite the sun with a small tilt/offset to avoid perfect symmetry.
  float H = (tod->timeHours - 12.0f) * (2.0f * PI / 24.0f) + PI + 0.1f;
  float decl = -SolarDeclination(tod->dayOfYear) + 0.05f;
  return CelestialDir(H, decl, tod->latitudeDeg);
}

float BrushTodSunElevation(const BrushTimeOfDay *tod) {
  return BrushTodSunDir(tod).y;
}

Vector3 BrushTodSunColor(const BrushTimeOfDay *tod) {
  return LookupColor(BrushTodSunElevation(tod), tod->sunColorLUT,
                     tod->sunColorCount);
}

Vector3 BrushTodMoonColor(const BrushTimeOfDay *tod) {
  Vector3 moonDir = BrushTodMoonDir(tod);
  if (moonDir.y <= 0.0f) return (Vector3){0, 0, 0};
  // Cool silver-blue moonlight, eased in with elevation. Soft (0.12 peak) so
  // the night stays night in the HDR pipeline.
  float f = moonDir.y;
  f = f * f * (3.0f - 2.0f * f);
  float intensity = f * 0.12f;
  return (Vector3){intensity * 0.75f, intensity * 0.85f, intensity * 1.0f};
}

Vector3 BrushTodAmbientColor(const BrushTimeOfDay *tod) {
  float e = BrushTodSunElevation(tod);
  Vector3 c = LookupColor(e, tod->ambientLUT, tod->ambientCount);
  float level = LookupFloat(e, tod->ambientLevelLUT, tod->ambientLevelCount);
  return Vector3Scale(c, level);
}

Vector3 BrushTodFogColor(const BrushTimeOfDay *tod) {
  return LookupColor(BrushTodSunElevation(tod), tod->fogColorLUT,
                     tod->fogColorCount);
}

float BrushTodExposure(const BrushTimeOfDay *tod) {
  return LookupFloat(BrushTodSunElevation(tod), tod->exposureLUT,
                     tod->exposureCount);
}
