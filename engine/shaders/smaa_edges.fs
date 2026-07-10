#version 330

// SMAA 1x — Pass 1/3: luma edge detection. Ported from Jimenez et al. SMAA.hlsl
// (MIT). Runs on the final LDR image; writes edge flags (R=west/left, G=north/top)
// so the next pass can measure the AA pattern. Diagonal/corner detection is
// omitted (the standard "medium" preset) to keep the port compact.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0; // color (LDR present)
uniform vec4 uMetrics;      // (1/w, 1/h, w, h)
uniform float uThreshold;   // SMAA_THRESHOLD (0.05..0.1)

#define LCAF 2.0            // local contrast adaptation factor

void main() {
    vec2 tc = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);
    vec2 t = uMetrics.xy;
    vec3 W = vec3(0.2126, 0.7152, 0.0722);

    float L      = dot(texture(texture0, tc).rgb, W);
    float Lleft  = dot(texture(texture0, tc + t * vec2(-1.0, 0.0)).rgb, W);
    float Ltop   = dot(texture(texture0, tc + t * vec2(0.0, -1.0)).rgb, W);

    vec4 delta;
    delta.xy = abs(L - vec2(Lleft, Ltop));
    vec2 edges = step(vec2(uThreshold), delta.xy);
    if (dot(edges, vec2(1.0)) == 0.0) discard;

    float Lright  = dot(texture(texture0, tc + t * vec2(1.0, 0.0)).rgb, W);
    float Lbottom = dot(texture(texture0, tc + t * vec2(0.0, 1.0)).rgb, W);
    delta.zw = abs(L - vec2(Lright, Lbottom));
    vec2 maxDelta = max(delta.xy, delta.zw);

    float Lleftleft = dot(texture(texture0, tc + t * vec2(-2.0, 0.0)).rgb, W);
    float Ltoptop   = dot(texture(texture0, tc + t * vec2(0.0, -2.0)).rgb, W);
    delta.zw = abs(vec2(Lleft, Ltop) - vec2(Lleftleft, Ltoptop));
    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    edges.xy *= step(vec2(finalDelta), LCAF * delta.xy);
    finalColor = vec4(edges, 0.0, 1.0);
}
