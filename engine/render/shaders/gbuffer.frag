#version 460
#extension GL_GOOGLE_include_directive : require
#define FRAGMENT_PASS
#define SURFACE_PASS
#include "common.glsl"
#include "raster_surface.glsl"
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPosition;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out float outViewZ;
layout(location = 5) out vec4 outEmission;
void main() {
    Instance i = instances[instanceIndex];
    SurfaceData surface = rasterSurface(i);
    vec3 n = normalize(surface.normal);
    outAlbedo = vec4(surface.albedo,surface.metallic);
    outNormal = vec4(n / max(max(abs(n.x),abs(n.y)),abs(n.z)),max(surface.roughness,.08));
    outPosition = vec4(worldPosition, float(i.info.z));
    vec2 currentUv = gl_FragCoord.xy / g.resolution.xy, prevUv = previousClip.xy / previousClip.w * .5 + .5;
    outMotion = vec4(prevUv - currentUv, previousClip.w, 0);
    outViewZ = abs((g.view * vec4(worldPosition, 1)).z);
    outEmission = vec4(surface.emission, 0);
}
