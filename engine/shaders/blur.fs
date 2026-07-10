#version 330

// Separable 9-tap Gaussian blur. Run once per axis (uDir = (1,0) then (0,1));
// ping-pong a few times for a wider, softer bloom.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;
uniform vec2 uDir;   // blur axis: (1,0) horizontal or (0,1) vertical
uniform vec2 uTexel; // 1.0 / texture size

void main() {
    // Gaussian weights (sigma ~2), widened slightly via the 1.5 tap spacing.
    float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec2 off = uDir * uTexel * 1.5;

    vec3 c = texture(texture0, fragTexCoord).rgb * w[0];
    for (int i = 1; i < 5; i++) {
        c += texture(texture0, fragTexCoord + off * float(i)).rgb * w[i];
        c += texture(texture0, fragTexCoord - off * float(i)).rgb * w[i];
    }
    finalColor = vec4(c, 1.0);
}
