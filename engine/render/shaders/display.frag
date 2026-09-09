#version 460
#extension GL_GOOGLE_include_directive : require
#define FRAGMENT_PASS
#define SURFACE_PASS
#include "common.glsl"
#include "raster_surface.glsl"
layout(location = 0) out vec4 outColor;
void main() {
    SurfaceData surface = rasterSurface(instances[instanceIndex]);
    outColor = vec4(surface.albedo + surface.emission, surface.opacity);
}
