#version 330

// brush forward lit pass — vertex.

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec4 vertexTangent; // xyz tangent, w handedness (glTF convention)

uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;

out vec3 fragPosition;
out vec2 fragTexCoord;
out vec3 fragNormal;
out vec4 fragColor;
out vec4 fragTangent;

void main()
{
    fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    // Meshes without tangents get a zero attribute; keep it unnormalized
    // (normalize(0) is undefined) — the fragment shader checks the length
    // and falls back to the geometric normal.
    fragTangent = vec4(vec3(matModel * vec4(vertexTangent.xyz, 0.0)),
                       vertexTangent.w);
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
