#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec3 inPreviousPosition;
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec4 previousClip;
layout(location = 3) flat out uint instanceIndex;
layout(location = 4) out vec3 vertexColor;
void main() {
    Instance i = instances[gl_InstanceIndex];
    vec4 p = i.model * vec4(inPosition, 1);
    worldPosition = p.xyz;
    worldNormal = transpose(inverse(mat3(i.model))) * inNormal;
    previousClip = g.previousViewProjection * i.previousModel * vec4(inPreviousPosition, 1);
    vertexColor = inColor;
    instanceIndex = gl_InstanceIndex;
    gl_Position = g.viewProjection * p;
}
