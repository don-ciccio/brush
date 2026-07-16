#version 330

// Screen-space ambient occlusion. Reconstructs view-space position from the
// scene depth, estimates a view normal from depth derivatives, then samples a
// randomly-rotated hemisphere kernel and counts how many samples are occluded
// by nearer geometry. Outputs an occlusion factor in R (1 = open, 0 = occluded).
// Kernel/quality follow the LearnOpenGL / r3d approach.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D uDepth;       // scene depth (window-space [0,1])
uniform sampler2D uNoise;       // 4x4 random rotation vectors
uniform vec3 uSamples[24];      // hemisphere kernel
uniform mat4 uProjection;       // camera projection
uniform mat4 uInvProjection;    // its inverse (reconstruct view pos)
uniform vec2 uNoiseScale;       // screen / 4, tiles the noise per pixel
uniform float uRadius;          // world-space sampling radius
uniform float uBias;            // self-occlusion bias
uniform float uStrength;        // occlusion strength

#define KERNEL_SIZE 24

vec3 viewFromDepth(vec2 uv, float d) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 v = uInvProjection * ndc;
    return v.xyz / v.w;
}

void main() {
    vec2 uv = fragTexCoord;
    float depth = texture(uDepth, uv).r;
    if (depth >= 1.0) { finalColor = vec4(1.0); return; } // background: no AO

    vec3 P = viewFromDepth(uv, depth);

    // View-space normal from depth derivatives, forced to face the camera.
    vec3 dpdx = dFdx(P);
    vec3 dpdy = dFdy(P);
    vec3 n = normalize(cross(dpdx, dpdy));
    if (n.z < 0.0) n = -n;

    // Foliage/Edge rejection: if the depth discontinuity across this pixel is
    // huge (like on overlapping alpha-tested grass strands), the reconstructed
    // normal is chaotic garbage and SSAO will produce massive black noise.
    float maxDerivative = max(length(dpdx), length(dpdy));
    float validNormal = smoothstep(uRadius * 1.5, uRadius * 0.5, maxDerivative);
    if (validNormal < 0.05) { finalColor = vec4(1.0); return; }

    // Per-pixel rotation from tiled noise → tangent basis (Gram-Schmidt).
    vec3 randomVec = normalize(vec3(texture(uNoise, uv * uNoiseScale).xy * 2.0 - 1.0, 0.0));
    vec3 tangent = normalize(randomVec - n * dot(randomVec, n));
    vec3 bitangent = cross(n, tangent);
    mat3 TBN = mat3(tangent, bitangent, n);

    float occlusion = 0.0;
    for (int i = 0; i < KERNEL_SIZE; i++) {
        vec3 samplePos = P + (TBN * uSamples[i]) * uRadius; // view space

        // Fast perspective projection to screen-space UV (replaces uProjection * vec4 matrix multiply)
        vec2 ndc_xy = vec2(
            uProjection[0][0] * samplePos.x + uProjection[2][0] * samplePos.z,
            uProjection[1][1] * samplePos.y + uProjection[2][1] * samplePos.z
        ) / -samplePos.z;
        vec2 sUV = ndc_xy * 0.5 + 0.5;
        
        if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0) continue;

        float sd = texture(uDepth, sUV).r;
        if (sd >= 1.0) continue;
        
        // Fast view-Z reconstruction (replaces costly viewFromDepth matrix multiply)
        float sceneZ = uProjection[3][2] / (1.0 - sd * 2.0 - uProjection[2][2]);

        // Occluded when real geometry sits nearer the camera than the sample,
        // range-checked so big depth gaps don't halo.
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sceneZ), 1e-4));
        occlusion += (sceneZ >= samplePos.z + uBias ? 1.0 : 0.0) * rangeCheck;
    }

    // Fade out SSAO in the distance to eliminate high-frequency noise on far terrain
    // -P.z is the view-space linear depth (since camera looks down -Z).
    // Fades starting at 40m, completely gone by 80m.
    float distanceFade = smoothstep(40.0, 80.0, -P.z);

    float ao = 1.0 - (occlusion / float(KERNEL_SIZE)) * uStrength * (1.0 - distanceFade);
    ao = mix(1.0, ao, validNormal);
    finalColor = vec4(vec3(clamp(ao, 0.0, 1.0)), 1.0);
}
