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

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec3 uSunDir;        // points toward the sun
uniform vec3 uSunColor;
uniform vec3 uAmbient; // ambient fill color (linear)
uniform vec3 viewPos;
uniform float uSpecStrength;
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
uniform sampler2D shadowMap0;
uniform sampler2D shadowMap1;
uniform sampler2D shadowMap2;
uniform vec3 uCascadeFar;      // cascade far distances from the camera (m)
uniform float uShadowEnabled;
uniform float uShadowSoftness; // light size in shadow texels
uniform float uShadowTexel;    // 1.0 / shadow map resolution
uniform float uShadowStrength; // horizon fade: 1 = full shadows, 0 = none

// PCSS test against one cascade. Returns 0 = fully lit, 1 = fully shadowed.
float ShadowSample(sampler2D shadowMap, mat4 lightVP, vec3 fragPos,
                   float ndotl) {
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
        float d = texture(shadowMap,
                          proj.xy + (vec2(x, y) - 1.5) * searchStep).r;
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
        if (receiver >
            texture(shadowMap, proj.xy + (vec2(x, y) - 1.5) * radius).r)
            sh += 1.0;
    return (sh / 16.0) * uShadowStrength;
}

// Cascade pick by radial camera distance (matches the sphere-fitted boxes).
float ShadowFactor(vec3 fragPos, float ndotl) {
    if (uShadowEnabled < 0.5) return 0.0;
    float d = length(fragPos - viewPos);
    if (d < uCascadeFar.x)
        return ShadowSample(shadowMap0, lightVP0, fragPos, ndotl);
    if (d < uCascadeFar.y)
        return ShadowSample(shadowMap1, lightVP1, fragPos, ndotl);
    if (d < uCascadeFar.z)
        return ShadowSample(shadowMap2, lightVP2, fragPos, ndotl);
    return 0.0; // beyond the last cascade: unshadowed
}

out vec4 finalColor;

void main()
{
    vec4 tex = texture(texture0, fragTexCoord);
    vec3 albedo = tex.rgb * colDiffuse.rgb * fragColor.rgb;
    if (uLinearize > 0.5) albedo = pow(albedo, vec3(2.2));

    vec3 N = normalize(fragNormal);
    vec3 L = normalize(uSunDir);
    float diff = max(dot(N, L), 0.0);

    // Sun visibility from the shadow map scales both direct terms. Fragments
    // facing away from the sun get no direct light at all — skip the 32-tap
    // PCSS entirely there (roughly half the pixels in a typical view).
    float sunVis = (diff > 0.0) ? 1.0 - ShadowFactor(fragPosition, diff) : 0.0;
    diff *= sunVis;

    vec3 V = normalize(viewPos - fragPosition);
    vec3 H = normalize(L + V);
    float spec = (diff > 0.0)
        ? pow(max(dot(N, H), 0.0), 48.0) * uSpecStrength * sunVis
        : 0.0;

    vec3 color = albedo * (uAmbient + uSunColor * diff) + uSunColor * spec;

    if (uLayerView == 1)      color = albedo;
    else if (uLayerView == 2) color = vec3(diff);
    else if (uLayerView == 3) color = vec3(spec);
    else if (uLayerView == 4) color = N * 0.5 + 0.5;
    else if (uLayerView == 5) color = vec3(sunVis);

    finalColor = vec4(color, tex.a * colDiffuse.a * fragColor.a);
}
