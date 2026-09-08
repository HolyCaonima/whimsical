#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 3) flat in uint instanceIndex;
layout(location = 0) out uint outEntityID;
void main() {
    outEntityID = instances[instanceIndex].info.z;
}
