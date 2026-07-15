#version 330

// brush forward lit pass — fragment.
//
// The image is composed from separable lighting terms:
//   albedo (color pass) -> diffuse sunlight -> specular highlight
// uLayerView isolates each term on screen (F2). When the HDR pipeline lands,
// these same terms become real intermediate render targets.

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec3 fragNormal;
in vec4 fragColor;
in vec4 fragTangent;
in vec3 localPosScaled;
in vec3 worldAxisX;
in vec3 worldAxisY;
in vec3 worldAxisZ;
in float vSwapUV;
in vec3 localNormal;

uniform sampler2D texture0; // MATERIAL_MAP_DIFFUSE (albedo)
uniform sampler2D texture2; // MATERIAL_MAP_NORMAL (raylib binds normals here)
uniform sampler2D texture4; // Surface Map (R=AO, G=Roughness, B=Height)
uniform vec4 colDiffuse;

// --- Per-draw material (set by the renderer from BrushMaterialProps) ---
// uTriplanar: sample albedo/normal by world position projected on the three
// axes, blended by the normal — scaled/rotated blockout geometry tiles in
// world space with no UV or tangent requirements. uTexScale is world metres
// per texture repeat. The UV path (models) uses the mesh tangents instead.
uniform float uTriplanar;
uniform float uTexScale;
uniform float uHasNormalMap;
uniform float uNormalDepth; // normal map intensity (1 = authored)
// 1 = the normal map is DXT5nm-swizzled (BC3: X in alpha, Y in green) —
// reconstruct Z instead of reading xyz directly.
uniform float uNormalSwizzled;
uniform float uHasSurfaceMap;
uniform float uHeightScale;
uniform float uParallax;      // 0 = flat (normal map only), 1 = parallax occlusion
uniform float uParallaxShadow; // 1 = soft self-shadow the height field toward the sun
uniform float uRoughnessDefault;
uniform float uAoStrength;

// --- Terrain splat (docs/terrain-painting-plan.md) ---
// Per-chunk RGBA8 weight texture blends up to 4 painted layers, sampled
// planar-XZ at each layer's own tile scale. Layer 0's albedo/normal ride
// texture0/texture2 (the material's maps); layers 1..3 sit on fixed units.
uniform float uSplatEnabled;
uniform sampler2D uSplatMap;
uniform sampler2D uLayerAlbedo1;
uniform sampler2D uLayerAlbedo2;
uniform sampler2D uLayerAlbedo3;
uniform sampler2D uLayerNormal1;
uniform sampler2D uLayerNormal2;
uniform sampler2D uLayerHeight;  // displacement of the ONE POM terrain layer
uniform int   uPomLayer;         // splat slot that gets POM (-1 = none)
uniform float uPomTile;          // that layer's tile
uniform float uPomScale;         // that layer's displacement scale
uniform int   uHeightBlendLayer; // splat slot that height-blends its edge (-1 = none)
uniform float uHeightBlendSharp; // transition band (smaller = crisper)
uniform vec2 uSplatOrigin; // chunk world origin (xz)
uniform float uSplatSize;  // chunk world size (m)
uniform float uSplatRes;   // splat texture resolution per side
uniform vec4 uLayerTiles;  // metres per repeat, per layer
uniform vec4 uLayerSwizzled; // DXT5nm flag per layer
uniform int uLayerCount;
uniform vec4 uLayerRoughness; // per-layer terrain roughness (0 smooth/shiny .. 1 matte)
// Grass-ground tint (foliage F3): the ground reads as a grass field where the
// foliage grow-layer sits, so the terrain doesn't end in bare dirt past the 3D
// blades. Pure uniform + reuses the splat weights (no sampler). strength 0 = off;
// growLayer -1 = everywhere; the tint ramps up to full by `far` metres so near
// ground keeps some of its own detail under the dense near grass.
uniform vec3  uGrassGroundColor;
uniform float uGrassGroundStrength;
uniform int   uGrassGrowLayer;
uniform float uGrassGroundFar;
uniform float uTerrainFarNormal; // 1 = Phase B (keep+amplify normal far), 0 = fade flat
// Road surface: an INDEPENDENT material composited over the finished terrain
// blend by a coverage mask, so it never enters the 4-layer mix or its auto-rules.
uniform float uRoadEnabled;
uniform sampler2D uRoadMask;   // R = road coverage 0..1
uniform sampler2D uRoadAlbedo;
uniform sampler2D uBiomeMap;   // R=id0 G=id1 B=blend (F2 BIOME debug view; unit 15)
uniform float uRoadTile;
// Road POM + height-blend reuse the terrain height sampler (uLayerHeight, unit
// 9): when the road material has a displacement map, its height is bound there
// and terrain-layer POM is disabled for the draw (the paving use case is now the
// road). Scalars only — no extra texture unit.
uniform float uRoadPom;         // 1 = parallax-occlusion the road surface
uniform float uRoadPomScale;    // road displacement scale
uniform float uRoadHeightBlend; // 1 = relief-follow the corridor edge
uniform float uRoadBlendSharp;  // edge transition band
uniform float uRoadHasHeight;   // 1 = road displacement bound -> derive its normal
// Auto-slope mask: steep terrain auto-blends toward one layer beneath the
// painted weights (x = layer index or -1 off, y = cos(startDeg),
// z = cos(endDeg); start < end in degrees so cosStart > cosEnd).
uniform vec3 uAutoSlope;
// Auto-height: one optional altitude band PER LAYER. Each layer fades in
// between its start and full Y (full > start = appears going up like a
// snowline; full < start = going down like a shoreline). Applied in slot
// order beneath the paint, before the slope mask. on/start/full packed as
// vec4s indexed by layer.
uniform vec4 uLayerHeightOn;    // 1 = this layer has a height band
uniform vec4 uLayerHeightStart;
uniform vec4 uLayerHeightFull;

uniform vec3 uSunDir;        // points toward the sun
uniform vec3 uSunColor;
uniform vec3 uAmbient; // ambient fill color (linear)
uniform vec3 viewPos;
uniform float uSpecStrength;

// --- Point lights (dynamic, shadowless — torches/lanterns) ---
#define MAX_POINT_LIGHTS 8
uniform vec3 uPointPos[MAX_POINT_LIGHTS];
uniform vec3 uPointColor[MAX_POINT_LIGHTS]; // linear, may exceed 1 (HDR)
uniform float uPointRadius[MAX_POINT_LIGHTS];
uniform int uPointCount;
uniform int uLayerView;
// 1 when the HDR post path is active: albedo inputs (textures/colors) are
// authored in sRGB, so decode them to linear here — the post composite
// gamma-encodes ONCE at the end. 0 on the direct LDR path (no post), where
// output stays in the authored space.
uniform float uLinearize;

// --- Cascaded sun shadows (PCSS: blocker search -> penumbra-widening PCF).
// Three ortho maps fitted around view-frustum slices; the fragment's camera
// distance picks the cascade. Same PCSS everywhere: one cascade is sampled
// per fragment, so the cost matches the old single-map path.
uniform mat4 lightVP0, lightVP1, lightVP2;
uniform sampler2D shadowAtlas; // all 3 cascades in one 2x2 depth atlas (1 unit)
uniform vec3 uCascadeFar;      // cascade far distances from the camera (m)
uniform float uShadowEnabled;
uniform float uShadowSoftness; // light size in shadow texels
uniform float uShadowTexel;    // 1.0 / shadow map resolution
uniform float uShadowStrength; // horizon fade: 1 = full shadows, 0 = none

// Sample the shadow atlas for cascade `tile` at cascade-space UV `uv`, clamped
// to the tile so PCF taps near a cascade edge don't bleed into a neighbour tile.
// An atlas texel is half a cascade texel (the tile spans half the 2x atlas).
float ShadowAtlasTap(vec2 tile, vec2 uv) {
    float at = uShadowTexel * 0.5;
    return texture(shadowAtlas, clamp(uv * 0.5 + tile, tile + at, tile + 0.5 - at)).r;
}

// PCSS test against one cascade (its `tile` origin in the atlas). 0 = lit, 1 = shadowed.
float ShadowSample(vec2 tile, mat4 lightVP, vec3 fragPos, float ndotl) {
    vec4 p = lightVP * vec4(fragPos, 1.0);
    vec3 proj = p.xyz / p.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0) return 0.0;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
        return 0.0;

    float bias = max(0.0015 * (1.0 - ndotl), 0.00015);
    float receiver = proj.z - bias;
    float lightSize = max(uShadowSoftness, 0.001);

    // Blocker search: average depth of occluders near this fragment.
    float searchStep = lightSize * uShadowTexel * 0.5;
    float blockerSum = 0.0, blockerCount = 0.0;
    for (int x = 0; x < 4; x++)
      for (int y = 0; y < 4; y++) {
        float d = ShadowAtlasTap(tile, proj.xy + (vec2(x, y) - 1.5) * searchStep);
        if (d < receiver) { blockerSum += d; blockerCount += 1.0; }
      }
    if (blockerCount < 0.5) return 0.0;

    // Penumbra widens with receiver-blocker distance (soft far, sharp near).
    float avgBlocker = blockerSum / blockerCount;
    float penumbra = (receiver - avgBlocker) / max(avgBlocker, 1e-4);
    float radius = clamp(penumbra * lightSize, 1.0, lightSize) * uShadowTexel;

    float sh = 0.0;
    for (int x = 0; x < 4; x++)
      for (int y = 0; y < 4; y++)
        if (receiver > ShadowAtlasTap(tile, proj.xy + (vec2(x, y) - 1.5) * radius))
            sh += 1.0;
    return (sh / 16.0) * uShadowStrength;
}

// Cascade pick by radial camera distance (matches the sphere-fitted boxes).
// Tiles follow the 2x2 atlas layout (BrushShadowCascadeTile in b_shadow.h).
float ShadowFactor(vec3 fragPos, float ndotl) {
    if (uShadowEnabled < 0.5) return 0.0;
    float d = length(fragPos - viewPos);
    if (d < uCascadeFar.x)
        return ShadowSample(vec2(0.0, 0.0), lightVP0, fragPos, ndotl);
    if (d < uCascadeFar.y)
        return ShadowSample(vec2(0.5, 0.0), lightVP1, fragPos, ndotl);
    if (d < uCascadeFar.z)
        return ShadowSample(vec2(0.0, 0.5), lightVP2, fragPos, ndotl);
    return 0.0; // beyond the last cascade: unshadowed
}

out vec4 finalColor;

// Blend weights for triplanar projection: dominant axes of the surface
// normal, sharpened so faces don't smear across each other.
vec3 TriplanarWeights(vec3 n) {
    vec3 w = pow(abs(n), vec3(4.0));
    return w / (w.x + w.y + w.z);
}

// Decode a sampled normal texel, DXT5nm-aware.
vec3 DecodeNormal(vec4 s, float swizzled) {
    if (swizzled > 0.5) {
        vec2 xy = s.ag * 2.0 - 1.0;
        return vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
    }
    return s.xyz * 2.0 - 1.0;
}

// World-space value-noise primitives. Shared by the terrain UV domain-warp
// (right below) and the procedural meadow colour further down.
float GG_Hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float GG_Noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(GG_Hash(i), GG_Hash(i + vec2(1, 0)), u.x),
               mix(GG_Hash(i + vec2(0, 1)), GG_Hash(i + vec2(1, 1)), u.x), u.y);
}
// Rotated sample (~37°) so the octaves don't align to the world grid and band.
float GG_Rot(vec2 p, float freq, float seed) {
    vec2 r = vec2(p.x * 0.7986 - p.y * 0.6019, p.x * 0.6019 + p.y * 0.7986);
    return GG_Noise(r * freq + seed);
}

// Low-frequency DOMAIN WARP: offset the sample position by a smooth noise so a
// tiled texture's regular grid never lines up into visible repeat bands at
// distance/grazing angle. RAMPED IN with distance so the near ground stays crisp
// (tiling only bands far away — like the donor, near sampling is left unwarped).
vec2 GG_WarpXZ(vec2 xz, float tile) {
    float ramp = smoothstep(15.0, 60.0, length(viewPos.xz - xz));
    if (ramp <= 0.0) return vec2(0.0);
    vec2 w = vec2(GG_Rot(xz, 0.08, 12.3), GG_Rot(xz, 0.08, 45.6)) - 0.5;
    return w * (tile * 0.3 * ramp);
}

// One splat layer's albedo: planar-XZ on flat ground (domain-warped to break the
// tile grid), full triplanar on steep surfaces (cliffs would smear a top proj).
vec3 SampleLayer(sampler2D tex, float tile, vec3 wp, vec3 triW, bool steep) {
    if (!steep) return texture(tex, (wp.xz + GG_WarpXZ(wp.xz, tile)) / tile).rgb;
    return texture(tex, wp.zy / tile).rgb * triW.x +
           texture(tex, wp.xz / tile).rgb * triW.y +
           texture(tex, wp.xy / tile).rgb * triW.z;
}

// One splat layer's tangent-space bump lifted to world axes (UDN swizzle),
// planar (domain-warped + distance mip-BIASed so the minified normal doesn't
// alias into bands) or triplanar to match SampleLayer.
vec3 SampleLayerBump(sampler2D tex, float tile, vec3 wp, vec3 triW,
                     float sw, bool steep, float bias) {
    if (!steep) {
        vec2 uv = (wp.xz + GG_WarpXZ(wp.xz, tile)) / tile;
        vec3 tn = DecodeNormal(texture(tex, uv, bias), sw);
        return vec3(tn.x, 0.0, tn.y);
    }
    vec3 tnX = DecodeNormal(texture(tex, wp.zy / tile), sw);
    vec3 tnY = DecodeNormal(texture(tex, wp.xz / tile), sw);
    vec3 tnZ = DecodeNormal(texture(tex, wp.xy / tile), sw);
    return vec3(0.0, tnX.y, tnX.x) * triW.x +
           vec3(tnY.x, 0.0, tnY.y) * triW.y +
           vec3(tnZ.x, tnZ.y, 0.0) * triW.z;
}

// Tangent-space normal fetch, DXT5nm-aware (see uNormalSwizzled).
vec3 SampleNormalMap(vec2 uv) {
    vec4 s = texture(texture2, uv);
    if (uNormalSwizzled > 0.5) {
        vec2 xy = s.ag * 2.0 - 1.0;
        return vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
    }
    return s.xyz * 2.0 - 1.0;
}

// Parallax Occlusion Mapping: ray-march the height field along the view ray in
// tangent space and return the offset UV where the ray meets the surface. The
// height map stores 1 = crest, so we walk depth 0->1. Offset-limited by the
// grazing angle; step count scales with the angle; textureGrad keeps the mip
// stable as the UV marches. `viewTS` points from the surface toward the eye.
vec2 ParallaxUV(sampler2D heightTex, vec2 uv, vec3 viewTS, float scale, bool readB) {
    float nSteps = mix(8.0, 32.0, clamp(1.0 - abs(viewTS.z), 0.0, 1.0));
    float layerD = 1.0 / nSteps;
    vec2 P = (viewTS.xy / max(abs(viewTS.z), 0.1)) * scale; // total sweep
    vec2 dUV = P * layerD;
    vec2 ddx = dFdx(uv), ddy = dFdy(uv);
    // Linear search: step along the ray until it passes under the height field.
    float curD = 0.0;
    vec2  cur = uv;
    float hCur = 1.0 - (readB ? textureGrad(heightTex, cur, ddx, ddy).b : textureGrad(heightTex, cur, ddx, ddy).r);
    for (int i = 0; i < 32; i++) {
        if (float(i) >= nSteps || curD >= hCur) break;
        cur  -= dUV;
        curD += layerD;
        hCur  = 1.0 - (readB ? textureGrad(heightTex, cur, ddx, ddy).b : textureGrad(heightTex, cur, ddx, ddy).r);
    }
    // Relief-mapping refinement: the intersection lies in the last [prev, cur]
    // interval (prev above the surface, cur below). Bisect it a few times for a
    // sharp hit — much crisper on stone edges than POM's single linear lerp.
    vec2  prev  = cur + dUV;
    float prevD = curD - layerD;
    for (int i = 0; i < 5; i++) {
        vec2  midUV = 0.5 * (cur + prev);
        float midD  = 0.5 * (curD + prevD);
        float hMid  = 1.0 - (readB ? textureGrad(heightTex, midUV, ddx, ddy).b : textureGrad(heightTex, midUV, ddx, ddy).r);
        if (midD >= hMid) { cur = midUV;  curD = midD; }  // still under: tighten near side
        else              { prev = midUV; prevD = midD; } // above: tighten far side
    }
    return 0.5 * (cur + prev);
}

// Soft self-shadow of the height field: march from the parallax hit point
// toward the light (tangent space) and accumulate how far the field pokes above
// the ray. 1 = lit, 0 = fully occluded (dark grout between raised stones). L
// points toward the light; scale matches ParallaxUV's depth.
float ParallaxShadow(sampler2D heightTex, vec2 uv, vec3 L, float scale, bool readB) {
    if (L.z <= 0.05) return 1.0;
    vec2 ddx = dFdx(uv), ddy = dFdy(uv);
    // Work in the same depth space as the march (0 = crest, 1 = valley). The
    // ray rises toward the light -> depth DECREASES by 1/N per step, matching
    // the horizontal (L.xy / L.z) * scale sweep (Tatarchuk's soft self-shadow).
    const int N = 12;
    float rayD = 1.0 - textureGrad(heightTex, uv, ddx, ddy).r; // hit depth
    vec2  dUV  = (L.xy / max(L.z, 0.1)) * scale / float(N);
    float dD   = 1.0 / float(N);
    float occ  = 0.0;
    vec2  cur  = uv;
    for (int i = 0; i < N; i++) {
        cur  += dUV;
        rayD -= dD;
        if (rayD <= 0.0) break;                          // above the tallest crest
        float sD = 1.0 - (readB ? textureGrad(heightTex, cur, ddx, ddy).b : textureGrad(heightTex, cur, ddx, ddy).r);
        if (sD < rayD) occ = max(occ, (rayD - sD) * (1.0 - float(i) / float(N)));
    }
    return 1.0 - clamp(occ * 4.0, 0.0, 1.0);
}

// ─── Procedural meadow ground colour (grass-ground tint) ────────────────────
// Ported from the donor's terrain.fs: a real grassland reads as a patchwork of
// greens/yellows at several spatial frequencies (biome regions, species
// clusters, fine tussocks), not a flat tint. Stacking rotated-noise layers
// makes the tinted terrain read as a grass field between/beyond the 3D blades —
// which also kills the 8-bit banding and resists the concentric-ring artefact
// (dense multi-scale detail has no smooth radial band to form a ring).
//
// Improvements over the donor: the whole palette is DERIVED from the layer's one
// groundColor (tunable, any grass — not racer's hard-coded meadow constants);
// the fine octave is fwidth-AA'd AND distance-gated so far pixels stay cheap.
// (GG_Hash/GG_Noise/GG_Rot are defined up by SampleLayer — shared with the warp.)
float GG_Biome(vec2 p)   { return GG_Rot(p, 0.035, 0.0) * 0.65 + GG_Rot(p, 0.09, 7.3) * 0.35; }
float GG_Species(vec2 p) { return GG_Rot(p, 0.18, 13.7) * 0.5 + GG_Rot(p, 0.55, 29.1) * 0.3 + GG_Rot(p, 1.40, 43.9) * 0.2; }
float GG_Clump(vec2 p)   { return GG_Rot(p, 2.8, 61.0) * 0.55 + GG_Rot(p, 7.5, 87.3) * 0.45; }
float GG_Drought(vec2 p) { return GG_Rot(vec2(p.x * 0.6, p.y), 0.12, 103.0); }

// `base` = layer groundColor (sRGB, pre-linearise, like the surrounding albedo).
// `gaze` = grazing-gap-hide factor (0 top-down .. hides bare soil at eye level).
vec3 GG_MeadowColor(vec3 base, vec2 wxz, float dist, float gaze) {
    // Palette derived from the one base tone. Kept SUBTLE and mostly in
    // BRIGHTNESS (same hue) — hue-shifted patches read as unnatural "spots".
    // straw/yellow are gentle warm accents, used sparingly.
    vec3 dark   = base * 0.82;
    vec3 light  = base * 1.16;
    vec3 lush   = base * 0.92;
    vec3 straw  = clamp(base * vec3(1.18, 1.12, 0.82), 0.0, 1.0);
    vec3 yellow = clamp(base * vec3(1.55, 1.40, 0.70), 0.0, 1.0);

    // Layer 1 — biome (large regional tone, 40-80 m): a GENTLE light/dark tonal
    // drift, with a rare, soft drier region. This is atmosphere, not patches.
    float biome = GG_Biome(wxz);
    vec3 col = mix(dark, light, smoothstep(0.15, 0.65, biome));
    col = mix(col, straw, smoothstep(0.58, 0.86, biome) * 0.25);

    // Layer 2 — species patches (5-15 m): the blotch risk. Keep the colour swing
    // small and the blend weight low so clusters read as faint tonal, not spots.
    float species = GG_Species(wxz);
    vec3 sp = mix(lush, light, smoothstep(0.28, 0.58, species));
    sp = mix(sp, yellow, smoothstep(0.82, 0.97, species) * 0.18); // sparse, soft
    col = mix(col, sp, 0.28);

    // Layer 3 — drought stress: minimal (elongated dry streaks read as stains).
    col = mix(col, straw, smoothstep(0.52, 0.82, GG_Drought(wxz)) * 0.12);

    // Layer 4 — fine clump (0.5-2 m tussocks): THIS is the grassy texture — the
    // realistic read is fine blade-scale detail, not mid-scale colour. fwidth-
    // AA'd + distance-gated so far pixels skip it (no shimmer, no cost).
    float footprint = max(fwidth(wxz.x), fwidth(wxz.y));
    float aaBase = 1.0 - smoothstep(0.45, 1.1, footprint * 2.8);
    if (aaBase > 0.0) {
        float clump = GG_Clump(wxz);
        float aaClump = 1.0 - smoothstep(0.15, 0.50, footprint * 7.5);
        vec3 clumpCol = mix(col * 0.84, col * 1.13, smoothstep(0.32, 0.64, clump));
        clumpCol = mix(clumpCol, yellow, smoothstep(0.86, 0.97, clump) * aaClump * 0.16);
        // Grazing gap-hide: a few dark inter-clump gaps top-down, closed at eye
        // level so the ground reads as solid grass cover.
        clumpCol = mix(clumpCol, dark, (1.0 - smoothstep(0.08, 0.20, clump)) * aaClump * gaze * 0.40);
        col = mix(col, clumpCol, aaBase * 0.70);
    }

    // (The far field's "living grass" read now comes from Phase B's normal-lit
    // shading + a tight grass SPECULAR sparkle in main() — light-driven, so it
    // blends and tracks the sun — instead of the old painted speckle dots.)

    // Distant grass fades a touch warmer (sun-bleached) — subtle.
    col = mix(col, yellow * 0.82, smoothstep(90.0, 220.0, dist) * 0.28);
    return col;
}

// False colour for the F2 BIOME debug view: a fixed 16-entry palette keyed by
// biome id (distinct hues so adjacent regions read apart). Debug only.
vec3 BiomeDebugColor(int id) {
    vec3 lut[16] = vec3[16](
        vec3(0.30,0.65,0.25), vec3(0.85,0.75,0.35), vec3(0.55,0.40,0.25),
        vec3(0.20,0.55,0.70), vec3(0.80,0.45,0.30), vec3(0.60,0.70,0.85),
        vec3(0.75,0.30,0.55), vec3(0.40,0.75,0.55), vec3(0.90,0.85,0.55),
        vec3(0.35,0.35,0.65), vec3(0.65,0.55,0.30), vec3(0.50,0.80,0.80),
        vec3(0.85,0.60,0.65), vec3(0.45,0.60,0.35), vec3(0.70,0.70,0.70),
        vec3(0.95,0.50,0.45));
    return lut[id & 15];
}

void main()
{
    vec3 geoN = normalize(fragNormal);
    float pomShadow = 1.0; // POM self-shadow, applied to the sun term below
    float gGrassMask = 0.0; // grass-ground coverage (set in the tint blocks) -> grass specular
    float gTerrainRough = 0.95; // per-layer weighted roughness (set in the splat block)
    vec3 triW = TriplanarWeights(geoN);
    float ts = max(uTexScale, 0.001);

    vec3 V = normalize(viewPos - fragPosition);

    // Default world-space UV coordinates (fallback if uTriplanar <= 0.5)
    vec2 uvMesh = fragTexCoord;

    // Local-space triplanar coordinates and weights
    vec2 uvX = vec2(0.0);
    vec2 uvY = vec2(0.0);
    vec2 uvZ = vec2(0.0);
    vec3 norm = normalize(localNormal);
    vec3 localPos = vec3(0.0);

    if (uTriplanar > 0.5) {
        triW = TriplanarWeights(norm);

        localPos = localPosScaled;

        // Project local coordinates along local axes
        uvX = localPos.zy / ts; // project along X
        if (vSwapUV > 0.5) {
            uvY = localPos.zx / ts; // project along Y, swapped to align bricks along Z length
        } else {
            uvY = localPos.xz / ts; // project along Y, standard to align bricks along X length
        }
        uvZ = localPos.xy / ts; // project along Z

        // Triplanar parallax: POM the DOMINANT axis only (1x, not 3x), faded
        // with distance. The other two axes are low-weight; leaving them
        // unoffset is invisible in the blend.
        if (uHasSurfaceMap > 0.5 && uParallax > 0.5) {
            float pomFade = 1.0 - smoothstep(12.0, 24.0, length(viewPos - fragPosition));
            if (pomFade > 0.0) {
                vec3 V_local = vec3(dot(V, worldAxisX),
                                    dot(V, worldAxisY),
                                    dot(V, worldAxisZ));
                float pScale = uHeightScale / ts; // world height -> projected-UV units
                if (triW.x >= triW.y && triW.x >= triW.z) {
                    vec3 vts = vec3(V_local.z, V_local.y, V_local.x); // uvX = zy, depth X
                    uvX = mix(uvX, ParallaxUV(texture4, uvX, vts, pScale, true), pomFade);
                } else if (triW.y >= triW.z) {
                    vec3 vts = (vSwapUV > 0.5) ? vec3(V_local.z, V_local.x, V_local.y)
                                               : vec3(V_local.x, V_local.z, V_local.y);
                    uvY = mix(uvY, ParallaxUV(texture4, uvY, vts, pScale, true), pomFade);
                } else {
                    vec3 vts = vec3(V_local.x, V_local.y, V_local.z); // uvZ = xy, depth Z
                    uvZ = mix(uvZ, ParallaxUV(texture4, uvZ, vts, pScale, true), pomFade);
                }
            }
        }
    } else {
        // Tangent-space parallax (mesh-UV materials / models). POM ray-march,
        // faded out with distance so far pixels pay nothing and don't shimmer.
        if (uHasSurfaceMap > 0.5 && uParallax > 0.5) {
            float pomFade = 1.0 - smoothstep(12.0, 24.0, length(viewPos - fragPosition));
            float tangentLen = length(fragTangent.xyz);
            if (pomFade > 0.0 && tangentLen > 0.1) {
                vec3 T = fragTangent.xyz / tangentLen;
                T = normalize(T - dot(T, geoN) * geoN);
                vec3 B = cross(geoN, T) * fragTangent.w;
                vec3 viewDirTS = vec3(dot(T, V), dot(B, V), dot(geoN, V));
                vec2 pomUV = ParallaxUV(texture4, fragTexCoord, viewDirTS, uHeightScale, true);
                uvMesh = mix(fragTexCoord, pomUV, pomFade);
                if (uParallaxShadow > 0.5) {
                    vec3 L = normalize(uSunDir);
                    vec3 lTS = vec3(dot(T, L), dot(B, L), dot(geoN, L));
                    pomShadow = mix(1.0, ParallaxShadow(texture4, pomUV, lTS, uHeightScale, true), pomFade);
                }
            }
        }
    }

    vec4 tex;
    if (uTriplanar > 0.5) {
        tex = texture(texture0, uvX) * triW.x +
              texture(texture0, uvY) * triW.y +
              texture(texture0, uvZ) * triW.z;
    } else {
        tex = texture(texture0, uvMesh);
    }
    vec3 albedo = tex.rgb * colDiffuse.rgb * fragColor.rgb;

    // Terrain splat: weights from the per-chunk texture (texel-centre
    // mapping — shared edge samples are identical across chunks, so
    // bilinear filtering is seamless at borders), layers blended planar-XZ.
    vec3 splatBump = vec3(0.0);
    if (uSplatEnabled > 0.5) {
        vec2 suv = ((fragPosition.xz - uSplatOrigin) / uSplatSize
                        * (uSplatRes - 1.0) + 0.5) / uSplatRes;
        vec4 sw = texture(uSplatMap, suv);
        sw /= max(sw.r + sw.g + sw.b + sw.a, 1e-4);

        // Painting is authoritative; auto-height/slope are a procedural BASE
        // for UNpainted ground only. `manual` = how far this pixel was painted
        // away from the default base coat (full layer 0), so a painted road /
        // path / rock isn't re-covered by an altitude or slope band. Cores of
        // painted areas keep their paint; the road/brush shoulder fade still
        // feathers the EDGES (half-painted -> half procedural), and untouched
        // terrain (manual = 0) gets the full masks.
        float autoMask = clamp(sw.r, 0.0, 1.0);

        // Per-layer auto-height bands, applied in slot order (higher slots
        // layer over lower ones), scaled by autoMask so paint wins.
        for (int hi = 0; hi < 4; hi++) {
            if (uLayerHeightOn[hi] < 0.5) continue;
            float s = uLayerHeightStart[hi];
            float fu = uLayerHeightFull[hi];
            float d = fu - s;
            if (abs(d) < 1e-4) d = (d < 0.0) ? -1e-4 : 1e-4;
            float f = clamp((fragPosition.y - s) / d, 0.0, 1.0);
            f = f * f * (3.0 - 2.0 * f);
            vec4 hw = vec4(hi == 0 ? 1.0 : 0.0, hi == 1 ? 1.0 : 0.0,
                           hi == 2 ? 1.0 : 0.0, hi == 3 ? 1.0 : 0.0);
            sw = mix(sw, hw, f * autoMask);
        }

        // Auto-slope: steepness pushes the weights toward one layer on
        // UNpainted ground (cos-angle smoothstep on the surface normal).
        if (uAutoSlope.x >= 0.0) {
            float denom = max(uAutoSlope.y - uAutoSlope.z, 1e-4);
            float f = clamp((uAutoSlope.y - geoN.y) / denom, 0.0, 1.0);
            f = f * f * (3.0 - 2.0 * f);
            vec4 slopeW = vec4(uAutoSlope.x == 0.0 ? 1.0 : 0.0,
                               uAutoSlope.x == 1.0 ? 1.0 : 0.0,
                               uAutoSlope.x == 2.0 ? 1.0 : 0.0,
                               uAutoSlope.x == 3.0 ? 1.0 : 0.0);
            sw = mix(sw, slopeW, f * autoMask);
        }

        // Height-based blend: reshape the flagged layer's weight by its own
        // height so its border with the surrounding terrain follows the relief
        // (raised stones persist, grass fills the grout) — a crisp, organic
        // edge instead of a linear fade or a straight cut. Only in the
        // transition band; the road's paintFade sets the band width.
        if (uHeightBlendLayer >= 0) {
            float wL = sw[uHeightBlendLayer];
            if (wL > 0.002 && wL < 0.998) {
                float hp = texture(uLayerHeight, fragPosition.xz / uPomTile).r;
                float depth = max(uHeightBlendSharp, 0.02);
                float mA = wL + hp;          // this layer: weight + surface height
                float mB = 1.0 - wL;         // everyone else, treated as flat
                float mx = max(mA, mB);
                float a = max(mA - mx + depth, 0.0);
                float b = max(mB - mx + depth, 0.0);
                float newWL = a / (a + b + 1e-5);
                float others = 1.0 - wL;
                sw *= (others > 1e-4) ? (1.0 - newWL) / others : 0.0;
                sw[uHeightBlendLayer] = newWL;
            }
        }

        // Per-layer roughness, weighted by the FINAL splat weights (sw is local
        // to this block, so it must be captured here for the lighting section).
        gTerrainRough = clamp(dot(sw, uLayerRoughness), 0.0, 1.0);

        // Steep surfaces get full triplanar per layer; flat ground keeps
        // the cheap single XZ projection.
        bool steep = (geoN.y < 0.85);
        vec3 wp = fragPosition;

        // Terrain-layer POM (e.g. a paving road): ray-march the ONE flagged
        // layer's displacement on FLAT ground (paved roads aren't cliffs) and
        // shift its sample position. Distance-faded; the other layers stay put.
        vec3 wpPom = wp;
        if (uPomLayer >= 0 && !steep) {
            float pomFade = 1.0 - smoothstep(12.0, 24.0, length(viewPos - fragPosition));
            if (pomFade > 0.0) {
                vec2 uv = wp.xz / uPomTile;                 // planar-XZ tile UV
                vec3 vts = vec3(V.x, V.z, V.y);             // world XZ tangent frame
                vec2 pomUV = ParallaxUV(uLayerHeight, uv, vts, uPomScale, false);
                wpPom.xz = wp.xz + (pomUV - uv) * uPomTile * pomFade;
                if (uParallaxShadow > 0.5) {
                    vec3 L = normalize(uSunDir);
                    vec3 lTS = vec3(L.x, L.z, L.y);
                    float sh = ParallaxShadow(uLayerHeight, pomUV, lTS, uPomScale, false);
                    float pomW = sw[uPomLayer]; // shadow only where the paving is
                    pomShadow = mix(1.0, sh, pomW * pomFade);
                }
            }
        }
        #define WP(i) ((uPomLayer == (i)) ? wpPom : wp)
        vec3 a = SampleLayer(texture0, uLayerTiles.x, WP(0), triW, steep) * sw.r;
        if (uLayerCount > 1) a += SampleLayer(uLayerAlbedo1, uLayerTiles.y, WP(1), triW, steep) * sw.g;
        if (uLayerCount > 2) a += SampleLayer(uLayerAlbedo2, uLayerTiles.z, WP(2), triW, steep) * sw.b;
        if (uLayerCount > 3) a += SampleLayer(uLayerAlbedo3, uLayerTiles.w, WP(3), triW, steep) * sw.a;
        albedo = a; // vertex colour/tint intentionally excluded

        // Grass-ground tint: blend the terrain toward the grass colour where the
        // foliage grows, ramping to full by uGrassGroundFar so near ground keeps
        // detail. Done BEFORE the road composite so roads still paint over it.
        if (uGrassGroundStrength > 0.001) {
            float gmask = (uGrassGrowLayer < 0) ? 1.0 : clamp(sw[uGrassGrowLayer], 0.0, 1.0);
            float gdist = length(viewPos - fragPosition);
            float gramp = mix(0.6, 1.0, smoothstep(0.0, max(uGrassGroundFar, 1.0) * 0.9, gdist));
            vec3 gcol = GG_MeadowColor(uGrassGroundColor, fragPosition.xz, gdist,
                                   max(normalize(viewPos - fragPosition).y, 0.0) * 0.8);
            a = mix(a, gcol, uGrassGroundStrength * gmask * gramp);
            gGrassMask = gmask; // grass coverage for the sparkle (before the road composite)
            albedo = a;
        }

        if (uHasNormalMap > 0.5) {
            // Push the normal to coarser mips with distance so the minified tile
            // stops aliasing (the ground for extending the normal further out).
            float nbias = clamp((length(viewPos - fragPosition) - 12.0) * 0.05, 0.0, 2.5);
            splatBump = SampleLayerBump(texture2, uLayerTiles.x, WP(0), triW, uLayerSwizzled.x, steep, nbias) * sw.r;
            if (uLayerCount > 1) splatBump += SampleLayerBump(uLayerNormal1, uLayerTiles.y, WP(1), triW, uLayerSwizzled.y, steep, nbias) * sw.g;
            if (uLayerCount > 2) splatBump += SampleLayerBump(uLayerNormal2, uLayerTiles.z, WP(2), triW, uLayerSwizzled.z, steep, nbias) * sw.b;        }
        #undef WP

        // Road surface: composite its OWN material over the finished terrain
        // blend by the coverage mask. Independent of the 4 layers above and of
        // auto-slope/auto-height, so it only appears on the road corridor.
        if (uRoadEnabled > 0.5) {
            // POM: ray-march the road's OWN displacement (bound to uLayerHeight
            // when the road material has one) and shift its sample position, so
            // the paving's displacement map produces real parallax depth.
            vec3 wpRoad = wp;
            if (uRoadPom > 0.5 && !steep) {
                float pomFade = 1.0 - smoothstep(12.0, 24.0, length(viewPos - fragPosition));
                if (pomFade > 0.0) {
                    vec2 uv = wp.xz / uRoadTile;
                    vec3 vts = vec3(V.x, V.z, V.y);
                    vec2 pomUV = ParallaxUV(uLayerHeight, uv, vts, uRoadPomScale, false);
                    wpRoad.xz = wp.xz + (pomUV - uv) * uRoadTile * pomFade;
                }
            }
            float rm = clamp(texture(uRoadMask, suv).r, 0.0, 1.0);
            // Height-blend: let the road surface relief drive the corridor edge so
            // the border follows mortar/cracks instead of a flat feather.
            if (uRoadHeightBlend > 0.5 && rm > 0.001 && rm < 0.999) {
                float h = texture(uLayerHeight, wpRoad.xz / uRoadTile).r;
                rm = clamp((rm - 1.0 + h) / max(uRoadBlendSharp, 0.05) + 0.5, 0.0, 1.0);
            }
            if (rm > 0.001) {
                a = mix(a, SampleLayer(uRoadAlbedo, uRoadTile, wpRoad, triW, steep), rm);
                albedo = a;
                gGrassMask *= (1.0 - rm); // roads paved over grass -> no grass sparkle
                // Surface normal: DERIVE it from the road's own displacement
                // (central-difference gradient of uLayerHeight) so the relief
                // catches the sun — highlights on the crowns, shade in the joints
                // — without a separate normal map / sampler. Falls back to flat
                // (inherit the terrain normal) when the road has no displacement.
                vec3 roadBump = vec3(0.0);
                if (uRoadHasHeight > 0.5) {
                    vec2 ruv = wpRoad.xz / uRoadTile;
                    float rdist = length(viewPos - fragPosition);
                    // Push the height samples to coarser mips with distance (like
                    // the terrain nbias) AND widen the central-difference footprint,
                    // so the derived normal low-passes instead of catching per-texel
                    // joint noise — that noise is what reads as fuzz on the paving.
                    float rbias = clamp((rdist - 8.0) * 0.06, 0.0, 3.0);
                    float e = 0.003 * (1.0 + rbias);
                    float hL = texture(uLayerHeight, ruv - vec2(e, 0.0), rbias).r;
                    float hR = texture(uLayerHeight, ruv + vec2(e, 0.0), rbias).r;
                    float hD = texture(uLayerHeight, ruv - vec2(0.0, e), rbias).r;
                    float hU = texture(uLayerHeight, ruv + vec2(0.0, e), rbias).r;
                    // Ease the relief toward flat far out so distant paving is smooth.
                    float bumpFade = 1.0 - smoothstep(24.0, 48.0, rdist);
                    roadBump = vec3(-(hR - hL), 0.0, -(hU - hD)) *
                               (uRoadPomScale * 25.0 * bumpFade);
                }
                splatBump = mix(splatBump, roadBump, rm);
            }
        }
    }
    // Zero-asset terrain (no splat layers): tint the plain/checker ground too,
    // so a meadow still reads as a grass field. (The splat path tints inside the
    // block above, grow-layer-masked and before the road composite.)
    if (uSplatEnabled <= 0.5 && uGrassGroundStrength > 0.001) {
        float gdist = length(viewPos - fragPosition);
        float gramp = mix(0.6, 1.0, smoothstep(0.0, max(uGrassGroundFar, 1.0) * 0.9, gdist));
        vec3 gcol = GG_MeadowColor(uGrassGroundColor, fragPosition.xz, gdist,
                                   max(normalize(viewPos - fragPosition).y, 0.0) * 0.8);
        albedo = mix(albedo, gcol, uGrassGroundStrength * gramp);
        gGrassMask = 1.0; // zero-asset ground is all grass -> sparkle everywhere
    }
    if (uLinearize > 0.5) albedo = pow(albedo, vec3(2.2));

    vec3 N = geoN;
    if (uSplatEnabled > 0.5) {
        float ndist = length(viewPos - fragPosition);
        float nscale;
        if (uTerrainFarNormal > 0.5) {
            // PHASE B: keep the normal at distance (the mip-bias makes it
            // alias-safe) and AMPLIFY it WHERE GRASS GROWS, so the far ground
            // shades like a grass field catching the sun — stronger far, and at
            // grazing angles to counteract perspective flattening. Non-grass
            // terrain (rock/dirt) keeps its authored normal. Faded only at the
            // extreme horizon (fixed distance — independent of the grass range).
            float grazing = 1.0 - max(normalize(viewPos - fragPosition).y, 0.0);
            float amp = mix(1.0, 3.2, smoothstep(20.0, 110.0, ndist)) *
                        mix(1.0, 1.5, grazing);
            amp = mix(1.0, amp, gGrassMask); // grass only; rock/dirt stay at 1.0
            float farKeep = 1.0 - smoothstep(400.0, 700.0, ndist);
            nscale = uNormalDepth * amp * farKeep;
        } else {
            // A: fade the bump to the flat geo normal by ~45 m (the pre-Phase-B
            // behaviour — far terrain lights flat, speckles carry the field).
            nscale = uNormalDepth * (1.0 - smoothstep(18.0, 45.0, ndist));
        }
        N = normalize(geoN + splatBump * nscale);
    } else if (uHasNormalMap > 0.5) {
        if (uTriplanar > 0.5) {
            // UDN blend in local space
            vec3 tnX = SampleNormalMap(uvX);
            vec3 tnY = SampleNormalMap(uvY);
            vec3 tnZ = SampleNormalMap(uvZ);
            
            // Construct tangent-space bump on local planes
            vec3 bump = vec3(0.0, tnX.y, tnX.x) * triW.x +
                        vec3(tnY.x, 0.0, tnY.y) * triW.y +
                        vec3(tnZ.x, tnZ.y, 0.0) * triW.z;
                        
            vec3 N_local = normalize(norm + bump * uNormalDepth);
            
            // Transform the bumped local normal back to world space
            N = normalize(N_local.x * worldAxisX +
                          N_local.y * worldAxisY +
                          N_local.z * worldAxisZ);
        } else {
            // Standard tangent-space path for models with authored UVs.
            float tangentLen = length(fragTangent.xyz);
            if (tangentLen > 0.1) {
                vec3 T = fragTangent.xyz / tangentLen;
                T = normalize(T - dot(T, geoN) * geoN);
                vec3 B = cross(geoN, T) * fragTangent.w;
                vec3 tn = SampleNormalMap(uvMesh);
                tn.xy *= uNormalDepth;
                N = normalize(mat3(T, B, geoN) * tn);
            }
        }
    }
    vec3 L = normalize(uSunDir);
    
    float isMatte = clamp(uSplatEnabled + uTriplanar, 0.0, 1.0);
    
    // Packed ORH fetching for Surface Maps
    float ao = 1.0;
    float roughness = uRoughnessDefault;
    if (uHasSurfaceMap > 0.5) {
        if (uTriplanar > 0.5) {
            vec4 surfX = texture(texture4, uvX);
            vec4 surfY = texture(texture4, uvY);
            vec4 surfZ = texture(texture4, uvZ);
            vec4 surf = surfX * triW.x + surfY * triW.y + surfZ * triW.z;
            ao = surf.r;
            roughness = surf.g;
        } else {
            vec4 surf = texture(texture4, uvMesh);
            ao = surf.r;
            roughness = surf.g;
        }
        ao = 1.0 - (1.0 - ao) * uAoStrength;
    }
    
    // Per-layer terrain roughness: shiny mud vs matte grass. Drives BOTH the
    // Oren-Nayar diffuse (via `roughness`) AND the specular (via `isMatte`, which
    // gates specPower/specStr below) — setting only `roughness` would leave every
    // terrain layer matte, since the specular reads isMatte, not roughness.
    if (uSplatEnabled > 0.5) {
        roughness = gTerrainRough;
        isMatte   = gTerrainRough;
    }
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    
    // Fast Oren-Nayar Diffuse
    float r2 = roughness * roughness;
    float A = 1.0 - 0.5 * (r2 / (r2 + 0.33));
    float B = 0.45 * (r2 / (r2 + 0.09));
    float LdotV = dot(L, V);
    float s = LdotV - NdotL * NdotV;
    float t = mix(1.0, max(NdotL, NdotV), step(0.0, s)) + 1e-6;
    float diff = NdotL * (A + B * s / t);

    // Sun visibility from the shadow map scales both direct terms. Fragments
    // facing away from the sun get no direct light at all — skip the 32-tap
    // PCSS entirely there (roughly half the pixels in a typical view).
    float sunVis = (NdotL > 0.0) ? 1.0 - ShadowFactor(fragPosition, NdotL) : 0.0;
    diff *= sunVis * pomShadow; // POM self-shadow darkens the sun term in the grout

    vec3 H = normalize(L + V);
    // Contextual Specular: Matte surfaces get a very weak, broad highlight. Models stay somewhat shiny.
    float specPower = mix(48.0, 4.0, isMatte);
    float specStr = uSpecStrength * mix(1.0, 0.05, isMatte);
    
    float spec = (NdotL > 0.0)
        ? pow(max(dot(N, H), 0.0), specPower) * specStr * sunVis
        : 0.0;

    // Point lights: inverse-square falloff with a smooth cutoff at the
    // radius (UE-style windowing) so influence reaches exactly zero — no
    // hard sphere edges, no lights popping at the boundary.
    vec3 pointDiff = vec3(0.0);
    vec3 pointSpec = vec3(0.0);
    for (int i = 0; i < uPointCount; i++) {
        vec3 toL = uPointPos[i] - fragPosition;
        float dist = length(toL);
        if (dist >= uPointRadius[i]) continue;
        vec3 Lp = toL / max(dist, 1e-4);
        float ratio = dist / uPointRadius[i];
        float window = 1.0 - ratio * ratio * ratio * ratio; // 1-(d/r)^4
        float att = (window * window) / (dist * dist + 1.0);
        float nl = max(dot(N, Lp), 0.0);
        pointDiff += uPointColor[i] * (nl * att);
        if (nl > 0.0) {
            vec3 Hp = normalize(Lp + V);
            pointSpec += uPointColor[i] *
                          (pow(max(dot(N, Hp), 0.0), 48.0) * uSpecStrength * att);
        }
    }



    // Grass sparkle: a TIGHT pinpoint specular off the (Phase-B amplified) grass
    // normal, so the field shimmers where blades catch the sun — continuous and
    // light-driven (warm at sunset, bright at noon), replacing the painted
    // speckle dots. High power = glints, not sheen; gated to the grass area and
    // sun visibility so shadowed grass stays matte. `spec` above stays near-matte.
    // Gated OFF near (3D grass covers it there, and the full-detail near normal
    // throws a broad washout at the sun reflection) — fades in with distance as a
    // far-field shimmer. Tight (high power) + low intensity = pinpoint glints.
    float sparkFade = smoothstep(uGrassGroundFar * 0.30, uGrassGroundFar * 0.75,
                                 length(viewPos - fragPosition));
    float grassSpec = (diff > 0.0 && gGrassMask > 0.0)
        ? pow(max(dot(N, H), 0.0), 220.0) * gGrassMask * uGrassGroundStrength * sunVis * sparkFade * 0.3
        : 0.0;

    // Hemispheric ambient: flat ambient light makes unlit sides look 2D and dimensionless.
    // The industry standard for a cheap volume fill is Hemispheric Lighting, blending
    // between a sky color (top) and a darker ground bounce color (bottom) based on the normal.
    // Here we derive the ground bounce color directly from the sky ambient color.
    // We use geoN.y instead of N.y so the ambient fill doesn't catch micro-facets from the
    // normal map, which causes high-frequency flickering during movement/animation.
    vec3 ambientGround = uAmbient * vec3(0.35, 0.38, 0.35); // darker, earthy tint
    vec3 hemiAmbient = mix(ambientGround, uAmbient, geoN.y * 0.5 + 0.5);

    vec3 color = albedo * (hemiAmbient * ao + uSunColor * diff + pointDiff) +
                 uSunColor * (spec + grassSpec) + pointSpec;

    if (uLayerView == 1)      color = albedo;
    else if (uLayerView == 2) color = vec3(diff);
    else if (uLayerView == 3) color = vec3(spec);
    else if (uLayerView == 4) color = N * 0.5 + 0.5;
    else if (uLayerView == 5) color = vec3(sunVis);
    else if (uLayerView == 6 && uSplatEnabled > 0.5) {
        // BIOME: false-colour the terrain map (NEAREST ids, blend the two debug
        // hues). Gated to terrain (uSplatEnabled) so props/player — which carry
        // no biome map — keep their normal shading instead of sampling garbage.
        vec2 buv = ((fragPosition.xz - uSplatOrigin) / uSplatSize
                        * (uSplatRes - 1.0) + 0.5) / uSplatRes;
        vec3 b = texture(uBiomeMap, buv).rgb;
        color = mix(BiomeDebugColor(int(b.r * 255.0 + 0.5)),
                    BiomeDebugColor(int(b.g * 255.0 + 0.5)), b.b);
    }

    finalColor = vec4(color, tex.a * colDiffuse.a * fragColor.a);
}
