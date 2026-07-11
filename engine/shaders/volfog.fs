#version 330

// Volumetric ground fog, ported from the donor codebase and generalized: the
// donor sampled its own terrain-noise function and carved a game-specific
// exclusion zone; here the fog volume is height-based relative to a flat
// uFogGroundY (games with tall terrain can tune density/top per scene).
//
// Marches the view ray from the camera up to the depth-buffer distance at
// half render resolution, accumulating an exponential height fog with
// FBM-noise coverage, wind drift, dual-lobe Henyey-Greenstein scattering and
// Interleaved Gradient Noise jitter. Output is straight-alpha, composited
// over the HDR scene BEFORE bloom/god rays so those passes see the fog.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D uDepth;  // scene depth
uniform mat4 uInvViewProj; // NDC -> world
uniform vec3 uCameraPos;
uniform vec3 uSunDir;   // direction TOWARD the sun
uniform vec3 uSunColor; // warm direct-light tint
uniform vec3 uSkyColor; // ambient fill colour for the fog body
uniform float uTime;
uniform vec2 uWindDir; // horizontal drift direction

uniform float uFogDensity;  // base extinction per metre
uniform float uFogGroundY;  // altitude where fog is densest (m)
uniform float uFogTopY;     // altitude (above ground) where fog fades out (m)
uniform float uFogCoverage; // 0..1 — higher = fog confined to fewer banks

// --- tunables ---------------------------------------------------------------
#define STEPS 16       // exponential step distribution
#define MAX_DIST 110.0 // fog saturates well before this; caps horizon cost
#define NEAR_START 1.2 // fog ignores the first NEAR_START m at the camera ...
#define NEAR_END 5.0   // ... and is fully present beyond NEAR_END m
// ----------------------------------------------------------------------------

float hash12(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float vnoise2D(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i + vec2(0.0, 0.0));
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// 3-octave FBM with cross-wind layers so the banks evolve instead of sliding.
float fbm2D(vec2 p, vec2 windA, vec2 windB) {
    return 0.5 * vnoise2D(p + windA)
         + 0.3 * vnoise2D(p * 2.03 + windB)
         + 0.2 * vnoise2D(p * 4.07 + windA * 2.0);
}

// Interleaved Gradient Noise — frame-stable screen-space dither (Jimenez).
float interleavedGradientNoise(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

float hgPhase(float cosA, float g) {
    float g2 = g * g;
    return (1.0 - g2) /
           (4.0 * 3.14159265 * pow(1.0 + g2 - 2.0 * g * cosA, 1.5));
}

void main() {
    float depth = texture(uDepth, fragTexCoord).r;

    // View ray for this pixel.
    vec4 ndcFar = vec4(fragTexCoord * 2.0 - 1.0, 1.0, 1.0);
    vec4 wf = uInvViewProj * ndcFar;
    vec3 rayDir = normalize(wf.xyz / wf.w - uCameraPos);

    // Distance to the rasterized surface (sky pixels march to MAX_DIST but
    // contribute no fog — see regionGate below).
    float skyW = clamp((depth - 0.9990) * 1030.9, 0.0, 1.0);
    bool isSky = (skyW >= 1.0);
    float sceneDist;
    vec2 groundXZ = vec2(0.0);
    if (isSky) {
        sceneDist = MAX_DIST;
    } else {
        vec4 ndcS = vec4(fragTexCoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec4 ws = uInvViewProj * ndcS;
        vec3 scenePos = ws.xyz / ws.w;
        sceneDist = distance(uCameraPos, scenePos);
        groundXZ = scenePos.xz;
    }

    // Prevent close geometry from carving black silhouettes, without forcing
    // the ground itself to glow from underground fog.
    float marchEnd = max(min(sceneDist, MAX_DIST), min(sceneDist, 2.0));

    // Fog-layer ceiling clamp for upward rays.
    float maxFogCeil = uFogGroundY + uFogTopY + 2.0;
    if (rayDir.y > 0.0001) {
        if (uCameraPos.y >= maxFogCeil) { finalColor = vec4(0.0); return; }
        marchEnd = min(marchEnd, (maxFogCeil - uCameraPos.y) / rayDir.y);
    }
    if (marchEnd < 0.25) { finalColor = vec4(0.0); return; }

    float cosA = dot(rayDir, uSunDir);

    // Dual-lobe Henyey-Greenstein (Frostbite/Hillaire): forward lobe for the
    // sun glow, weak back lobe so fog never goes black looking away.
    float phase = mix(hgPhase(cosA, 0.8), hgPhase(cosA, -0.3), 0.25);

    vec3 wind = vec3(uWindDir.x, 0.0, uWindDir.y) * uTime * 0.8;
    vec2 windA = wind.xz * 0.012;
    vec2 windB = wind.xz * 0.007 * vec2(-0.6, 0.8);

    float transmittance = 1.0;
    vec3 scatter = vec3(0.0);

    float jitter = interleavedGradientNoise(gl_FragCoord.xy);

    for (int i = 0; i < STEPS; i++) {
        // Quadratic step distribution: more samples near the camera where
        // fog detail matters (UE5 volumetric fog, Wronski AC4).
        float fi = (float(i) + jitter) / float(STEPS);
        float t = marchEnd * fi * fi;
        float stepLen = marchEnd * (2.0 * fi + 1.0 / float(STEPS)) / float(STEPS);
        vec3 pos = uCameraPos + rayDir * t;

        // 1. Horizontal coverage from turbulence FBM.
        float field = fbm2D(pos.xz * 0.012, windA, windB);
        float coverage = smoothstep(uFogCoverage, uFogCoverage + 0.38, field);
        if (coverage <= 0.0) continue;

        // 2. Exponential height falloff above the fog ground plane.
        float heightFactor = max(pos.y - uFogGroundY, 0.0);
        float localTopY = uFogTopY + (field - 0.5) * 1.5;
        float h = exp(-heightFactor / max(localTopY, 0.3));

        // 3. Analytic micro-wisps (cheaper than another noise octave).
        float detail =
            0.6 + 0.4 * sin(pos.x * 0.45 - wind.x) * cos(pos.z * 0.45 - wind.z);

        float dens = uFogDensity * h * coverage * detail;
        dens *= smoothstep(NEAR_START, NEAR_END, t);
        if (dens <= 0.0) continue;

        // Multi-scattering ambient approximation (Frostbite/Hillaire): soft
        // omnidirectional fill so fog in shadow doesn't go black.
        vec3 desatSun =
            mix(vec3(dot(uSunColor, vec3(0.299, 0.587, 0.114))), uSunColor, 0.65);
        float ambientScatter = 0.15;
        vec3 lit = uSkyColor * ambientScatter
                 + (uSkyColor + desatSun * (phase * 1.5)) * (1.0 - ambientScatter);

        float dT = exp(-dens * stepLen);
        scatter += transmittance * (1.0 - dT) * lit;
        transmittance *= dT;
        if (transmittance < 0.02) break;
    }

    // No fog on sky/gap pixels (kills horizon-haze speckle through geometry
    // gaps), and a gentle distance fade hides the coverage banks' hard
    // spatial edges when seen from afar.
    float regionGate = isSky ? 0.0 : 1.0;
    float distToFog = length(uCameraPos.xz - groundXZ);
    float distFade = isSky ? 0.0 : smoothstep(MAX_DIST, MAX_DIST * 0.55, distToFog);
    float alpha = clamp(1.0 - transmittance, 0.0, 1.0) * regionGate * distFade;
    finalColor = vec4(scatter, alpha);
}
