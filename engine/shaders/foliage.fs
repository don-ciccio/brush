#version 330

// brush instanced foliage — fragment. Alpha-tested cards lit by the engine's
// single sun + ambient, with optional CSM shadowing (cheap 1-tap; grass is low,
// heavily overdrawn, and alpha-tested, so PCSS isn't worth the fetches). Outputs
// LINEAR HDR — the post pass tonemaps/exposes, and depth-based fog in the
// composite hazes distant foliage for free, so this shader does neither.

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec3 fragNormal;
in vec4 fragColor;      // .a = blade height (0 base, 1 tip); .rgb typically white
in float fragDist;
in vec3 fragMacroColor;
in float fragFade;

uniform sampler2D texture0; // albedo / gradient card
uniform vec4 colDiffuse;
uniform vec3 uSunDir;   // points toward the sun
uniform vec3 uSunColor;
uniform vec3 uAmbient;  // ambient fill (linear)
uniform vec3 viewPos;
uniform float uLinearize; // 1 = sRGB albedo -> linear (post path); 0 = direct LDR
uniform vec3 uGrassTint;  // per-layer albedo tint (1,1,1 = none)
uniform float uAlphaCutoff; // discard below this texture alpha (card cutout)
uniform float uImpostor;    // 1 = billboard: output the pre-lit baked atlas as-is

// Sun shadow (CSM) — mirrors lit.fs's cascade selection, single tap. Gated off
// by default (uShadowEnabled 0) so a shader with no shadow maps bound still runs.
uniform mat4 lightVP0, lightVP1, lightVP2;
uniform sampler2D shadowMap0, shadowMap1, shadowMap2;
uniform vec3 uCascadeFar;      // cascade far distances (m)
uniform float uShadowEnabled;  // 1 = sample shadows
uniform float uShadowStrength; // horizon fade (1 = full, 0 = none)

out vec4 finalColor;

// One hard tap with slope-scaled bias. 1 = shadowed, 0 = lit.
float ShadowTap(sampler2D sm, mat4 lightVP, vec3 fragPos, float ndotl) {
    vec4 p = lightVP * vec4(fragPos, 1.0);
    vec3 proj = p.xyz / p.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0) return 0.0;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) return 0.0;
    float bias = max(0.002 * (1.0 - ndotl), 0.0005);
    return (proj.z - bias > texture(sm, proj.xy).r) ? 1.0 : 0.0;
}

float ShadowFactor(vec3 fragPos, float ndotl) {
    if (fragDist < uCascadeFar.x) return ShadowTap(shadowMap0, lightVP0, fragPos, ndotl);
    if (fragDist < uCascadeFar.y) return ShadowTap(shadowMap1, lightVP1, fragPos, ndotl);
    if (fragDist < uCascadeFar.z) return ShadowTap(shadowMap2, lightVP2, fragPos, ndotl);
    return 0.0;
}

void main() {
    vec4 tex = texture(texture0, fragTexCoord);
    if (tex.a < uAlphaCutoff) discard;

    // Billboard impostor: the atlas already holds the clump's fully-lit LINEAR
    // colour (baked once from the 3D mesh with the scene sun), so output it
    // directly — re-lighting a flat card can't reproduce the mesh's averaged
    // per-blade shading and reads too dark. Just apply the distance fade.
    if (uImpostor > 0.5) { finalColor = vec4(tex.rgb, tex.a * fragFade * colDiffuse.a); return; }

    vec3 albedo = tex.rgb * colDiffuse.rgb * fragColor.rgb * uGrassTint;
    albedo *= fragMacroColor; // low-frequency patch variation
    if (uLinearize > 0.5) albedo = pow(albedo, vec3(2.2));

    // Half-Lambert (Valve wrap): grass cards are thin and two-sided, so shade
    // view-independently and never let a face go fully black. This avoids the
    // hard left/right seam a view-dependent normal flip produces.
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(uSunDir);
    float raw = dot(N, L);
    float wrap = raw * 0.5 + 0.5;
    float diffuse = wrap * wrap;

    float sunVis = 1.0;
    if (uShadowEnabled > 0.5)
        sunVis = 1.0 - ShadowFactor(fragPosition, max(raw, 0.0)) * uShadowStrength;

    vec3 color = albedo * (uAmbient + uSunColor * diffuse * sunVis);

    // Linear HDR out; alpha carries the distance fade for a soft cull edge.
    finalColor = vec4(max(color, 0.0), tex.a * fragFade * colDiffuse.a);
}
