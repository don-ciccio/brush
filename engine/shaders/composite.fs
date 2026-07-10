#version 330

// HDR composite — the single place tone mapping happens.
// Pipeline: add bloom -> exposure -> ACES tone map -> colour grade (ASC CDL
// split toning) -> film contrast pivot -> vibrance/saturation -> vignette ->
// film grain -> optional Display P3 gamut map -> sRGB gamma encode.
//
// Samples with flipped Y (render textures store bottom-up); the final
// upscale blit flips back, so the parity nets out upright.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0; // HDR scene (linear, un-tonemapped)
uniform sampler2D uBloom;   // half-res multi-scale bloom
uniform sampler2D uAO;      // blurred SSAO (R: 1 = open, 0 = occluded)
uniform float uAOEnabled;
uniform float uBloomIntensity;
uniform float uExposure;   // pre-tonemap multiplier
uniform vec2 uResolution;  // output resolution (vignette aspect + grain)
uniform float uTime;       // animates the grain dither
uniform float uDisplayP3;  // 1 = apply the sRGB->P3 gamut map (Apple wide-gamut)
uniform float uP3Strength; // 1 = accurate map, lower = punchier native P3

// Filmic ACES (Narkowicz) — expects LINEAR input.
vec3 ACESFilm(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Vibrance: saturation boost weighted toward less-saturated colors, so it
// never pushes already-vivid pixels into neon.
vec3 AdjustVibrance(vec3 color, float amount) {
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float sat = max(color.r, max(color.g, color.b)) -
                min(color.r, min(color.g, color.b));
    return max(mix(vec3(luma), color, 1.0 + amount * (1.0 - sat)), 0.0);
}

void main() {
    vec2 uv = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);

    // 1. Ambient occlusion on the scene (not on bloom — bloom is light
    //    bleed), then bloom add (linear HDR) + exposure.
    vec3 hdr = texture(texture0, uv).rgb;
    hdr *= mix(1.0, texture(uAO, uv).r, uAOEnabled);
    hdr += texture(uBloom, uv).rgb * uBloomIntensity;
    hdr *= uExposure;

    // 2. Tone map — ACES on full-range HDR.
    vec3 col = ACESFilm(hdr);

    // 3. Colour grade (split toning, ASC CDL): neutral shadows, warm golden
    //    highlights.
    vec3 slope = vec3(1.01, 1.00, 0.98);
    col = max(col * slope, 0.0);

    // 4. Film contrast pivot — subtle cinematic punch.
    col = (col - 0.35) * 0.97 + 0.35;
    col = clamp(col, 0.0, 1.0);

    // 5. Vibrance + gentle global saturation.
    col = AdjustVibrance(col, 0.08);
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(lum), col, 1.12);

    // 6. Vignette — soft aspect-corrected ellipse (squared distance, no sqrt).
    vec2 vc = uv - 0.5;
    vc.x *= uResolution.x / uResolution.y;
    float vig = 1.0 - smoothstep(0.18, 0.72, dot(vc, vc));
    col *= mix(0.72, 1.0, vig);

    // 7. Film grain — ~0.1%, faded out in deep shadows/highlights so it never
    //    reads as noise or banding.
    float grain = hash12(uv * uResolution + fract(uTime) * 1000.0);
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float grainIntensity = 0.001 * smoothstep(0.0, 0.18, luma) *
                           (1.0 - smoothstep(0.82, 1.0, luma));
    col += (grain - 0.5) * grainIntensity;

    // 8. Display P3 gamut map: untagged GL output on Apple wide-gamut screens
    //    displays sRGB values as native P3 (oversaturated). Blend toward the
    //    accurate map by uP3Strength.
    if (uDisplayP3 > 0.5) {
        vec3 p3 = vec3(dot(col, vec3(0.8225, 0.1775, 0.0000)),
                       dot(col, vec3(0.0332, 0.9669, 0.0000)),
                       dot(col, vec3(0.0171, 0.0724, 0.9108)));
        col = mix(col, p3, uP3Strength);
    }

    // 9. Gamma-encode to display sRGB (P3 shares the same curve).
    col = pow(max(col, 0.0), vec3(1.0 / 2.2));
    finalColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
