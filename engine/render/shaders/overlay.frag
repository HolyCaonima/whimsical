#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 3) flat in uint instanceIndex;
layout(location = 4) in vec3 vertexColor;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 tint = unpackUnorm4x8(instances[instanceIndex].info.w).yzw;
    outColor = vec4(vertexColor * tint, 1);
}
