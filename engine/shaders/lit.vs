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
out vec3 localPosScaled;
out vec3 worldAxisX;
out vec3 worldAxisY;
out vec3 worldAxisZ;
out float vSwapUV;
out vec3 localNormal;

void main()
{
    fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragTangent = vec4(vec3(matModel * vec4(vertexTangent.xyz, 0.0)),
                       vertexTangent.w);

    // Compute rotation matrix R (columns are matModel basis vectors)
    mat3 R;
    R[0] = normalize(matModel[0].xyz);
    R[1] = normalize(matModel[1].xyz);
    R[2] = normalize(matModel[2].xyz);

    // Project world position back to the unrotated frame
    localPosScaled = transpose(R) * fragPosition;

    // Pass the local normal (unrotated normal) to the fragment shader
    localNormal = vertexNormal;

    // Compute world-space directions of local axes for normal mapping transform
    worldAxisX = R[0];
    worldAxisY = R[1];
    worldAxisZ = R[2];

    // Compute scales along local axes for UV swap logic
    float scaleX = length(matModel[0].xyz);
    float scaleZ = length(matModel[2].xyz);
    vSwapUV = (scaleZ > scaleX) ? 1.0 : 0.0;

    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
