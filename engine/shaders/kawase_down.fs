#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;
uniform vec2 uTexel; // 1.0 / target texture size

void main() {
    // 13-tap Kawase downsample, relies on bilinear filtering to sample 4 pixels at once
    vec2 uv = fragTexCoord;
    vec2 halfPixel = uTexel * 0.5;

    vec3 sum = texture(texture0, uv).rgb * 4.0;
    sum += texture(texture0, uv - halfPixel).rgb;
    sum += texture(texture0, uv + halfPixel).rgb;
    sum += texture(texture0, uv + vec2(halfPixel.x, -halfPixel.y)).rgb;
    sum += texture(texture0, uv - vec2(halfPixel.x, -halfPixel.y)).rgb;

    finalColor = vec4(sum * 0.125, 1.0);
}
