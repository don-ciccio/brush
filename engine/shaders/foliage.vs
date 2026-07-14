#version 330

// brush instanced foliage — vertex. Ported from the donor grass.vs, stripped of
// the game-specific player-push / trampling-trail interaction (roadmap v1 #8).
// Keeps: per-instance distance fade (height shrink), distance size shrink, wind
// sway, and a low-frequency macro-color variation evaluated once per vertex.

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;      // .a = height along the blade (0 base, 1 tip)
in mat4 instanceTransform; // per-instance world matrix (raylib instancing attr)

uniform mat4 mvp;          // view * projection (model is per-instance)
uniform float uTime;
uniform vec2 uWindDirection;
uniform float uWindStrength;
uniform vec3 viewPos;

// Distance fade window (match the layer's CPU cull distances so instances fade
// to nothing exactly as they are culled — no pop). uFadeEnd == drawDistance.
uniform float uFadeStart;
uniform float uFadeEnd;
uniform float uFadeNearStart; // fade-IN begins here — grow from the ground
uniform float uFadeNearEnd;   // if > 0, fade IN completes here (far/billboard bands)

// Macro-color ramp endpoints (data-driven per layer; neutral greens by default).
uniform vec3 uMacroLow;
uniform vec3 uMacroHigh;

out vec3 fragPosition;
out vec2 fragTexCoord;
out vec3 fragNormal;
out vec4 fragColor;
out float fragDist;
out vec3 fragMacroColor;
out float fragFade;

// Bhaskara I sine approximation — cheap, smooth enough for sway.
float fastSin(float x) {
    x = mod(x + 3.14159265, 6.28318530) - 3.14159265;
    float ax = abs(x);
    return x * (3.14159265 - ax) / (3.14159265 + 0.405 * ax * ax - ax);
}

// Dave Hoskins' hash-without-sine + value noise + 3-octave fBm for patchiness.
float Hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float ValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash21(i);
    float b = Hash21(i + vec2(1.0, 0.0));
    float c = Hash21(i + vec2(0.0, 1.0));
    float d = Hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float MacroNoise(vec2 p) {
    p *= 4.0;
    float n = ValueNoise(p * 0.2) * 0.5;
    n += ValueNoise(p * 0.8) * 0.25;
    n += ValueNoise(p * 2.5) * 0.125;
    return clamp((n - 0.4375) * 2.28 * 0.5 + 0.5, 0.0, 1.0); // remap to 0..1
}

void main() {
    // Instance base in world space (the patch root).
    vec4 baseWorld = instanceTransform * vec4(0.0, 0.0, 0.0, 1.0);
    float instDist = length(viewPos - baseWorld.xyz);

    // Distance fade (S-curve to 0 at the cull edge) and optional near fade-in.
    float fadeNorm = clamp((instDist - uFadeStart) / max(uFadeEnd - uFadeStart, 0.001), 0.0, 1.0);
    float grassFade = smoothstep(1.0, 0.0, fadeNorm);
    float nearFade = 1.0;
    if (uFadeNearEnd > 0.0) {
        nearFade = clamp((instDist - uFadeNearStart) / max(uFadeNearEnd - uFadeNearStart, 0.001), 0.0, 1.0);
    }
    float heightScale = grassFade * nearFade;

    // Shrink distant instances to cut overdraw (100% -> 35% between 100..250 m).
    float sizeScale = mix(1.0, 0.35, clamp((instDist - 100.0) / 150.0, 0.0, 1.0));
    // Fade/shrink by HEIGHT only: scaling all axes would slide the base verts
    // toward the model origin as the camera distance changes, making the grass
    // look like it drifts. Shrinking Y keeps the base footprint planted.
    vec3 localPos = vertexPosition;
    localPos.y *= sizeScale * heightScale;
    vec4 worldPos = instanceTransform * vec4(localPos, 1.0);

    // Wind: lean each vertex horizontally in the wind direction by an amount
    // proportional to its height above the instance base — 0 at the base (so the
    // clump stays planted) growing toward the tips, and it scales with the
    // model's own size. A per-instance phase makes neighbours ripple. This is a
    // pure bend, NOT a rotation about the origin, so wide patch meshes don't
    // shear/swing — the base never moves, which is what keeps grass from
    // appearing to drift as the camera moves.
    float h = max(worldPos.y - baseWorld.y, 0.0);
    float phase = baseWorld.x * 0.22 + baseWorld.z * 0.22 + uTime * 1.3;
    float gust = fastSin(phase) * uWindStrength;
    float lean = gust * 0.15 * h;
    worldPos.x += uWindDirection.x * lean;
    worldPos.z += uWindDirection.y * lean;
    worldPos.y -= abs(lean) * 0.2; // slight droop as the blade bends over

    fragPosition = worldPos.xyz;
    fragTexCoord = vertexTexCoord;
    fragNormal = normalize(mat3(instanceTransform) * vertexNormal);
    fragColor = vertexColor;
    fragDist = length(viewPos - worldPos.xyz);
    fragFade = heightScale;
    fragMacroColor = mix(uMacroLow, uMacroHigh, MacroNoise(baseWorld.xz));

    gl_Position = mvp * worldPos;
}
