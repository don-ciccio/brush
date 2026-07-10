#version 330

// SMAA 1x — Pass 2/3: blending-weight calculation (orthogonal only). Ported from
// SMAA.hlsl (MIT). For each edge pixel it searches along the edge for the crossing
// edges, then looks up the pre-computed AreaTex to get how much of the pixel the
// smooth (anti-aliased) line covers. Diagonal + corner refinements are omitted.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0; // edges (RG)
uniform sampler2D uArea;    // AreaTex (RG, 160x560, bilinear)
uniform sampler2D uSearch;  // SearchTex (R, 64x16)
uniform vec4 uMetrics;      // (1/w, 1/h, w, h)

#define MAX_SEARCH_STEPS 16
#define AREATEX_MAX_DISTANCE 16.0
#define AREATEX_PIXEL_SIZE (1.0 / vec2(160.0, 560.0))
#define AREATEX_SUBTEX_SIZE (1.0 / 7.0)
#define SEARCHTEX_SIZE vec2(66.0, 33.0)
#define SEARCHTEX_PACKED_SIZE vec2(64.0, 16.0)

float searchLength(vec2 e, float offset) {
    vec2 scale = SEARCHTEX_SIZE * vec2(0.5, -1.0);
    vec2 bias  = SEARCHTEX_SIZE * vec2(offset, 1.0);
    scale += vec2(-1.0, 1.0);
    bias  += vec2(0.5, -0.5);
    scale *= 1.0 / SEARCHTEX_PACKED_SIZE;
    bias  *= 1.0 / SEARCHTEX_PACKED_SIZE;
    return textureLod(uSearch, scale * e + bias, 0.0).r;
}

float searchXLeft(vec2 tc, float end) {
    vec2 e = vec2(0.0, 1.0);
    for (int i = 0; i < MAX_SEARCH_STEPS; i++) {
        if (!(tc.x > end && e.g > 0.8281 && e.r == 0.0)) break;
        e = textureLod(texture0, tc, 0.0).rg;
        tc -= vec2(2.0, 0.0) * uMetrics.xy;
    }
    float offset = -(255.0 / 127.0) * searchLength(e, 0.0) + 3.25;
    return uMetrics.x * offset + tc.x;
}
float searchXRight(vec2 tc, float end) {
    vec2 e = vec2(0.0, 1.0);
    for (int i = 0; i < MAX_SEARCH_STEPS; i++) {
        if (!(tc.x < end && e.g > 0.8281 && e.r == 0.0)) break;
        e = textureLod(texture0, tc, 0.0).rg;
        tc += vec2(2.0, 0.0) * uMetrics.xy;
    }
    float offset = -(255.0 / 127.0) * searchLength(e, 0.5) + 3.25;
    return -uMetrics.x * offset + tc.x;
}
float searchYUp(vec2 tc, float end) {
    vec2 e = vec2(1.0, 0.0);
    for (int i = 0; i < MAX_SEARCH_STEPS; i++) {
        if (!(tc.y > end && e.r > 0.8281 && e.g == 0.0)) break;
        e = textureLod(texture0, tc, 0.0).rg;
        tc -= vec2(0.0, 2.0) * uMetrics.xy;
    }
    float offset = -(255.0 / 127.0) * searchLength(e.gr, 0.0) + 3.25;
    return uMetrics.y * offset + tc.y;
}
float searchYDown(vec2 tc, float end) {
    vec2 e = vec2(1.0, 0.0);
    for (int i = 0; i < MAX_SEARCH_STEPS; i++) {
        if (!(tc.y < end && e.r > 0.8281 && e.g == 0.0)) break;
        e = textureLod(texture0, tc, 0.0).rg;
        tc += vec2(0.0, 2.0) * uMetrics.xy;
    }
    float offset = -(255.0 / 127.0) * searchLength(e.gr, 0.5) + 3.25;
    return -uMetrics.y * offset + tc.y;
}

vec2 area(vec2 dist, float e1, float e2) {
    vec2 tc = AREATEX_MAX_DISTANCE * round(4.0 * vec2(e1, e2)) + dist;
    tc = AREATEX_PIXEL_SIZE * tc + 0.5 * AREATEX_PIXEL_SIZE;
    // subsample offset is 0 for SMAA 1x
    return textureLod(uArea, tc, 0.0).rg;
}

void main() {
    vec2 tc = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);
    vec4 m = uMetrics;
    vec2 pixcoord = tc * m.zw;

    vec4 off0 = tc.xyxy + m.xyxy * vec4(-0.25, -0.125, 1.25, -0.125);
    vec4 off1 = tc.xyxy + m.xyxy * vec4(-0.125, -0.25, -0.125, 1.25);
    vec4 off2 = vec4(off0.xz, off1.yw) +
                vec4(m.xx, m.yy) * vec4(-2.0, 2.0, -2.0, 2.0) * float(MAX_SEARCH_STEPS);

    vec4 weights = vec4(0.0);
    vec2 e = texture(texture0, tc).rg;

    if (e.g > 0.0) { // edge at north
        vec3 coords;
        coords.x = searchXLeft(off0.xy, off2.x);
        coords.y = off1.y;
        float e1 = textureLod(texture0, coords.xy, 0.0).r;
        coords.z = searchXRight(off0.zw, off2.y);
        vec2 d = abs(round(m.zz * vec2(coords.x, coords.z) - pixcoord.xx));
        vec2 sqrt_d = sqrt(d);
        float e2 = textureLodOffset(texture0, vec2(coords.z, coords.y), 0.0, ivec2(1, 0)).r;
        weights.rg = area(sqrt_d, e1, e2);
    }

    if (e.r > 0.0) { // edge at west
        vec3 coords;
        coords.y = searchYUp(off1.xy, off2.z);
        coords.x = off0.x;
        float e1 = textureLod(texture0, coords.xy, 0.0).g;
        coords.z = searchYDown(off1.zw, off2.w);
        vec2 d = abs(round(m.ww * vec2(coords.y, coords.z) - pixcoord.yy));
        vec2 sqrt_d = sqrt(d);
        float e2 = textureLodOffset(texture0, vec2(coords.x, coords.z), 0.0, ivec2(0, 1)).g;
        weights.ba = area(sqrt_d, e1, e2);
    }

    finalColor = weights;
}
