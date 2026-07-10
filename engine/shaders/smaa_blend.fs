#version 330

// SMAA 1x — Pass 3/3: neighborhood blending. Ported from SMAA.hlsl (MIT). Uses the
// per-pixel blend weights to mix each pixel with the neighbour across the edge,
// exploiting bilinear filtering so the coverage lookup becomes a smooth line.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0; // color (LDR present, bilinear)
uniform sampler2D uBlend;   // blend weights (RGBA)
uniform vec4 uMetrics;      // (1/w, 1/h, w, h)

void main() {
    vec2 tc = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);
    vec4 m = uMetrics;
    vec4 off = tc.xyxy + m.xyxy * vec4(1.0, 0.0, 0.0, -1.0);

    vec4 a;
    a.x = texture(uBlend, off.xy).a; // right
    a.y = texture(uBlend, off.zw).g; // top
    a.wz = texture(uBlend, tc).xz;   // bottom (x), left (z)

    if (dot(a, vec4(1.0)) < 1e-5) {
        finalColor = textureLod(texture0, tc, 0.0);
    } else {
        bool h = max(a.x, a.z) > max(a.y, a.w); // horizontal blend dominates?

        vec4 blendingOffset = vec4(0.0, a.y, 0.0, a.w);
        vec2 blendingWeight = a.yw;
        blendingOffset = mix(blendingOffset, vec4(a.x, 0.0, a.z, 0.0), bvec4(h));
        blendingWeight = mix(blendingWeight, a.xz, bvec2(h));
        blendingWeight /= dot(blendingWeight, vec2(1.0));

        vec4 blendingCoord = tc.xyxy + blendingOffset * vec4(m.xy, -m.xy);
        vec4 color = blendingWeight.x * textureLod(texture0, blendingCoord.xy, 0.0);
        color += blendingWeight.y * textureLod(texture0, blendingCoord.zw, 0.0);
        finalColor = color;
    }
}
