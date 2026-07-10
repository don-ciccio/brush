#version 330

// Sky dome vertex shader. The dome is a unit-ish sphere centered on the camera,
// so the raw object-space vertex position IS the view direction — pass it
// straight through and let the fragment shader normalize per-pixel.

in vec3 vertexPosition;

uniform mat4 mvp;

out vec3 fragDir;

void main()
{
    fragDir = vertexPosition;
    // Force depth to the far plane (z = w -> ndc z = 1.0) so the dome can be
    // drawn LAST and rejected by early-Z everywhere the terrain already wrote
    // depth. raylib's depth func is GL_LEQUAL, so it still fills uncovered sky.
    // This means the costly cloud fbm only runs on actually-visible sky pixels.
    vec4 clip = mvp * vec4(vertexPosition, 1.0);
    gl_Position = clip.xyww;
}
