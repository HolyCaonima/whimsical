#version 460
#extension GL_GOOGLE_include_directive : require
#define FRAGMENT_PASS
#define SURFACE_PASS
#include "common.glsl"
#include "raster_surface.glsl"
layout(location = 0) out uint outEntityID;
void main() {
    Instance i = instances[instanceIndex];
    rasterSurface(i);
    outEntityID = i.info.z;
}
