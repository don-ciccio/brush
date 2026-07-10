#version 330

// 4x4 box blur that matches the 4x4 SSAO noise tile, removing the dithering
// pattern the per-pixel kernel rotation introduces.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0; // raw AO
uniform vec2 uTexel;        // 1 / resolution

void main() {
    float sum = 0.0;
    for (int x = -2; x <= 1; x++)
        for (int y = -2; y <= 1; y++)
            sum += texture(texture0, fragTexCoord + vec2(float(x), float(y)) * uTexel).r;
    finalColor = vec4(vec3(sum / 16.0), 1.0);
}
