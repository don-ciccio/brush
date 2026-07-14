#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;
uniform vec2 uTexel; // 1.0 / source texture size

void main() {
    // 9-tap Kawase upsample, relies on bilinear filtering
    vec2 uv = fragTexCoord;
    vec2 halfPixel = uTexel * 0.5;

    vec3 sum = texture(texture0, uv + vec2(-halfPixel.x * 2.0, 0.0)).rgb;
    sum += texture(texture0, uv + vec2(-halfPixel.x, halfPixel.y)).rgb * 2.0;
    sum += texture(texture0, uv + vec2(0.0, halfPixel.y * 2.0)).rgb;
    sum += texture(texture0, uv + vec2(halfPixel.x, halfPixel.y)).rgb * 2.0;
    sum += texture(texture0, uv + vec2(halfPixel.x * 2.0, 0.0)).rgb;
    sum += texture(texture0, uv + vec2(halfPixel.x, -halfPixel.y)).rgb * 2.0;
    sum += texture(texture0, uv + vec2(0.0, -halfPixel.y * 2.0)).rgb;
    sum += texture(texture0, uv + vec2(-halfPixel.x, -halfPixel.y)).rgb * 2.0;

    finalColor = vec4(sum * 0.0833333, 1.0); // 1.0 / 12.0
}
