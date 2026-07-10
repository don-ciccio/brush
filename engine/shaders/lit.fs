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
uniform float uAmbient;
uniform vec3 viewPos;
uniform float uSpecStrength;
uniform int uLayerView;
// 1 when the HDR post path is active: albedo inputs (textures/colors) are
// authored in sRGB, so decode them to linear here — the post composite
// gamma-encodes ONCE at the end. 0 on the direct LDR path (no post), where
// output stays in the authored space.
uniform float uLinearize;      // 0 final, 1 albedo, 2 diffuse, 3 specular, 4 normals

out vec4 finalColor;

void main()
{
    vec4 tex = texture(texture0, fragTexCoord);
    vec3 albedo = tex.rgb * colDiffuse.rgb * fragColor.rgb;
    if (uLinearize > 0.5) albedo = pow(albedo, vec3(2.2));

    vec3 N = normalize(fragNormal);
    vec3 L = normalize(uSunDir);
    float diff = max(dot(N, L), 0.0);

    vec3 V = normalize(viewPos - fragPosition);
    vec3 H = normalize(L + V);
    float spec = (diff > 0.0)
        ? pow(max(dot(N, H), 0.0), 48.0) * uSpecStrength
        : 0.0;

    vec3 color = albedo * (uAmbient + uSunColor * diff) + uSunColor * spec;

    if (uLayerView == 1)      color = albedo;
    else if (uLayerView == 2) color = vec3(diff);
    else if (uLayerView == 3) color = vec3(spec);
    else if (uLayerView == 4) color = N * 0.5 + 0.5;

    finalColor = vec4(color, tex.a * colDiffuse.a * fragColor.a);
}
