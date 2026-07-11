#version 330

// Volumetric light scattering (god rays), ported from the donor codebase
// (Dagon-engine approach): raymarch the world-space view ray and sample the
// sun shadow map to accumulate optical depth, weighted by a Henyey-Greenstein
// phase function for anisotropic forward scattering. Runs at quarter render
// resolution into an HDR target; a separable blur afterwards smooths the
// raymarch dither, and the composite adds the result over the scene.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;   // HDR scene (drives fragTexCoord only)
uniform sampler2D uDepth;     // scene depth
uniform sampler2D uShadowMap; // directional sun shadow map

uniform mat4 uInvViewProj;
uniform mat4 uMatLight;
uniform vec3 uCameraPos;
uniform vec3 uSunDir; // direction toward the sun
uniform vec3 uSunColor;

uniform vec2 uResolution;
uniform float uTime;

uniform float uGodRaysDensity;  // scattering density multiplier
uniform float uGodRaysWeight;   // Henyey-Greenstein anisotropy g (~0.85)
uniform float uGodRaysExposure; // overall intensity
uniform float uGodRaysDecay;    // max ray length in units of 100 m

#define PI 3.14159265359

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Mie scattering approximated with the Henyey-Greenstein phase function.
float hgScattering(float lightDotView, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * lightDotView;
    return (1.0 - g2) / (4.0 * PI * pow(denom, 1.5));
}

void main() {
    float depth = texture(uDepth, fragTexCoord).r;

    // Unproject to world space (fragTexCoord is the flipped render-texture
    // orientation, matching the depth lookup).
    vec4 ndc = vec4(fragTexCoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = uInvViewProj * ndc;
    vec3 worldPos = wp.xyz / wp.w;

    vec3 rayVector = worldPos - uCameraPos;
    float sceneDist = length(rayVector);
    vec3 rayDir = rayVector / sceneDist;

    float maxDist = min(sceneDist, uGodRaysDecay * 100.0);
    if (maxDist < 0.1) {
        finalColor = vec4(0.0);
        return;
    }

    const int NUM_SAMPLES = 24;
    float stepSize = maxDist / float(NUM_SAMPLES);
    float invSamples = 1.0 / float(NUM_SAMPLES);

    // Dither the march start to hide banding (blurred out afterwards).
    float offset = hash12(fragTexCoord * uResolution + fract(uTime));

    vec3 currentPos = uCameraPos + rayDir * (stepSize * offset);
    float opticalDepth = 0.0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
        vec4 shadowPos = uMatLight * vec4(currentPos, 1.0);
        vec3 projCoords = shadowPos.xyz / shadowPos.w;
        projCoords = projCoords * 0.5 + 0.5;

        if (projCoords.x >= 0.0 && projCoords.x <= 1.0 &&
            projCoords.y >= 0.0 && projCoords.y <= 1.0 &&
            projCoords.z >= 0.0 && projCoords.z <= 1.0) {
            float closestDepth = texture(uShadowMap, projCoords.xy).r;
            if (projCoords.z - 0.001 <= closestDepth) {
                opticalDepth += 1.0; // this sample sees the sun
            }
        }
        currentPos += rayDir * stepSize;
    }

    opticalDepth *= invSamples;

    float cosTheta = dot(rayDir, uSunDir);
    float phase = hgScattering(cosTheta, uGodRaysWeight);

    vec3 radiance =
        uSunColor * opticalDepth * phase * uGodRaysDensity * uGodRaysExposure * 5.0;

    // Scale by the distance actually traveled through the medium: short rays
    // (ground near the camera) get weak shafts, horizon/sky rays get full.
    float distanceFade = 1.0 - exp(-maxDist * 0.035);

    // Damp shafts projected onto solid geometry so surfaces don't wash out;
    // fade in smoothly near the horizon depth to avoid seams.
    float isSky = smoothstep(0.9990, 0.9999, depth);
    float fadeScale = mix(0.20, 1.0, isSky);

    radiance *= distanceFade * fadeScale;

    // Soft fade near the camera to avoid harsh clipping.
    radiance *= smoothstep(0.0, 2.0, sceneDist);

    finalColor = vec4(radiance, 1.0);
}
