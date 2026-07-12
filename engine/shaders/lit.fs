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
uniform sampler2D texture4; // MATERIAL_MAP_OCCLUSION (ao)
uniform sampler2D texture6; // MATERIAL_MAP_HEIGHT (displacement)
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
uniform float uHasHeightMap;
uniform float uHeightScale;
uniform float uHasAoMap;
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
uniform sampler2D uLayerNormal3;
uniform vec2 uSplatOrigin; // chunk world origin (xz)
uniform float uSplatSize;  // chunk world size (m)
uniform float uSplatRes;   // splat texture resolution per side
uniform vec4 uLayerTiles;  // metres per repeat, per layer
uniform vec4 uLayerSwizzled; // DXT5nm flag per layer
uniform int uLayerCount;
// Auto-slope mask: steep terrain auto-blends toward one layer beneath the
// painted weights (x = layer index or -1 off, y = cos(startDeg),
// z = cos(endDeg); start < end in degrees so cosStart > cosEnd).
uniform vec3 uAutoSlope;

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

// One splat layer's albedo: planar-XZ on flat ground, full triplanar on
// steep surfaces (sculpted cliffs would smear a top-projected texture).
vec3 SampleLayer(sampler2D tex, float tile, vec3 wp, vec3 triW, bool steep) {
    if (!steep) return texture(tex, wp.xz / tile).rgb;
    return texture(tex, wp.zy / tile).rgb * triW.x +
           texture(tex, wp.xz / tile).rgb * triW.y +
           texture(tex, wp.xy / tile).rgb * triW.z;
}

// One splat layer's tangent-space bump lifted to world axes (UDN swizzle),
// planar or triplanar to match SampleLayer.
vec3 SampleLayerBump(sampler2D tex, float tile, vec3 wp, vec3 triW,
                     float sw, bool steep) {
    if (!steep) {
        vec3 tn = DecodeNormal(texture(tex, wp.xz / tile), sw);
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

void main()
{
    vec3 geoN = normalize(fragNormal);
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

        if (uHasHeightMap > 0.5) {
            // Project view direction V into local space
            vec3 V_local = vec3(dot(V, worldAxisX),
                                dot(V, worldAxisY),
                                dot(V, worldAxisZ));

            float hX = texture(texture6, uvX).r;
            float hY = texture(texture6, uvY).r;
            float hZ = texture(texture6, uvZ).r;
            float h = hX * triW.x + hY * triW.y + hZ * triW.z;

            // Offset the local coordinates along the local view vector
            vec3 offsetPos = localPos - V_local * ((1.0 - h) * uHeightScale);
            uvX = offsetPos.zy / ts;
            if (vSwapUV > 0.5) {
                uvY = offsetPos.zx / ts;
            } else {
                uvY = offsetPos.xz / ts;
            }
            uvZ = offsetPos.xy / ts;
        }
    } else {
        if (uHasHeightMap > 0.5) {
            float tangentLen = length(fragTangent.xyz);
            if (tangentLen > 0.1) {
                vec3 T = fragTangent.xyz / tangentLen;
                T = normalize(T - dot(T, geoN) * geoN);
                vec3 B = cross(geoN, T) * fragTangent.w;
                float h = texture(texture6, fragTexCoord).r;
                vec3 viewDirTS = vec3(dot(T, V), dot(B, V), dot(geoN, V));
                float denom = max(viewDirTS.z, 0.1);
                uvMesh = fragTexCoord - (viewDirTS.xy / denom) * ((1.0 - h) * uHeightScale);
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

        // Auto-slope: steepness pushes the weights toward one layer
        // beneath the paint (cos-angle smoothstep on the surface normal).
        if (uAutoSlope.x >= 0.0) {
            float denom = max(uAutoSlope.y - uAutoSlope.z, 1e-4);
            float f = clamp((uAutoSlope.y - geoN.y) / denom, 0.0, 1.0);
            f = f * f * (3.0 - 2.0 * f);
            vec4 slopeW = vec4(uAutoSlope.x == 0.0 ? 1.0 : 0.0,
                               uAutoSlope.x == 1.0 ? 1.0 : 0.0,
                               uAutoSlope.x == 2.0 ? 1.0 : 0.0,
                               uAutoSlope.x == 3.0 ? 1.0 : 0.0);
            sw = mix(sw, slopeW, f);
        }

        // Steep surfaces get full triplanar per layer; flat ground keeps
        // the cheap single XZ projection.
        bool steep = (geoN.y < 0.85);
        vec3 wp = fragPosition;
        vec3 a = SampleLayer(texture0, uLayerTiles.x, wp, triW, steep) * sw.r;
        if (uLayerCount > 1) a += SampleLayer(uLayerAlbedo1, uLayerTiles.y, wp, triW, steep) * sw.g;
        if (uLayerCount > 2) a += SampleLayer(uLayerAlbedo2, uLayerTiles.z, wp, triW, steep) * sw.b;
        if (uLayerCount > 3) a += SampleLayer(uLayerAlbedo3, uLayerTiles.w, wp, triW, steep) * sw.a;
        albedo = a; // vertex colour/tint intentionally excluded

        if (uHasNormalMap > 0.5) {
            splatBump = SampleLayerBump(texture2, uLayerTiles.x, wp, triW, uLayerSwizzled.x, steep) * sw.r;
            if (uLayerCount > 1) splatBump += SampleLayerBump(uLayerNormal1, uLayerTiles.y, wp, triW, uLayerSwizzled.y, steep) * sw.g;
            if (uLayerCount > 2) splatBump += SampleLayerBump(uLayerNormal2, uLayerTiles.z, wp, triW, uLayerSwizzled.z, steep) * sw.b;
            if (uLayerCount > 3) splatBump += SampleLayerBump(uLayerNormal3, uLayerTiles.w, wp, triW, uLayerSwizzled.w, steep) * sw.a;
        }
    }
    if (uLinearize > 0.5) albedo = pow(albedo, vec3(2.2));

    vec3 N = geoN;
    if (uSplatEnabled > 0.5) {
        N = normalize(geoN + splatBump * uNormalDepth);
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
    float diff = max(dot(N, L), 0.0);

    // Sun visibility from the shadow map scales both direct terms. Fragments
    // facing away from the sun get no direct light at all — skip the 32-tap
    // PCSS entirely there (roughly half the pixels in a typical view).
    float sunVis = (diff > 0.0) ? 1.0 - ShadowFactor(fragPosition, diff) : 0.0;
    diff *= sunVis;

    vec3 H = normalize(L + V);
    float spec = (diff > 0.0)
        ? pow(max(dot(N, H), 0.0), 48.0) * uSpecStrength * sunVis
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

    // Ambient Occlusion
    float ao = 1.0;
    if (uHasAoMap > 0.5) {
        if (uTriplanar > 0.5) {
            float aoX = texture(texture4, uvX).r;
            float aoY = texture(texture4, uvY).r;
            float aoZ = texture(texture4, uvZ).r;
            ao = aoX * triW.x + aoY * triW.y + aoZ * triW.z;
        } else {
            ao = texture(texture4, uvMesh).r;
        }
        ao = 1.0 - (1.0 - ao) * uAoStrength;
    }

    vec3 color = albedo * (uAmbient * ao + uSunColor * diff + pointDiff) +
                 uSunColor * spec + pointSpec;

    if (uLayerView == 1)      color = albedo;
    else if (uLayerView == 2) color = vec3(diff);
    else if (uLayerView == 3) color = vec3(spec);
    else if (uLayerView == 4) color = N * 0.5 + 0.5;
    else if (uLayerView == 5) color = vec3(sunVis);

    finalColor = vec4(color, tex.a * colDiffuse.a * fragColor.a);
}
