#version 330

// Bloom bright-pass: keep only the portion of each pixel above the luminance
// threshold (preserving hue), so HDR highlights bloom and the rest stays dark.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0; // HDR scene
uniform float uThreshold;

void main() {
    vec3 c = texture(texture0, fragTexCoord).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Soft over-threshold contribution, hue-preserving.
    float contribution = max(l - uThreshold, 0.0) / max(l, 1e-5);
    finalColor = vec4(c * contribution, 1.0);
}
