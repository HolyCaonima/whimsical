#version 460
#extension GL_GOOGLE_include_directive : require
#define FRAGMENT_PASS
#define SURFACE_PASS
#include "common.glsl"
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec4 previousClip;
layout(location = 3) flat in uint instanceIndex;
layout(location = 4) in vec3 vertexColor;
layout(location = 5) in vec2 texcoord;
layout(location = 6) in vec4 worldTangent;
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPosition;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out float outViewZ;
layout(location = 5) out vec4 outEmission;
void main() {
    Instance i = instances[instanceIndex];
    MaterialContext ctx;
    ctx.material = i.info.x;
    ctx.position = worldPosition;
    ctx.normal = normalize(worldNormal) * (gl_FrontFacing ? 1 : -1);
    ctx.tangent = worldTangent;
    ctx.vertexColor = vertexColor;
    ctx.uv = texcoord;
    ctx.viewDirection = normalize(g.eyeTime.xyz-worldPosition);
    ctx.frontFacing = gl_FrontFacing;
    ctx.textureLod = 0;
    ctx.footprint = max(length(dFdx(worldPosition)),length(dFdy(worldPosition)));
    SurfaceData surface = EvaluateSurface(ctx);
    if (!AcceptSurface(ctx,surface)) discard;
    vec3 n = normalize(surface.normal);
    outAlbedo = vec4(surface.albedo,surface.metallic);
    outNormal = vec4(n / max(max(abs(n.x),abs(n.y)),abs(n.z)),max(surface.roughness,.08));
    outPosition = vec4(worldPosition, float(i.info.z));
    vec2 currentUv = gl_FragCoord.xy / g.resolution.xy, prevUv = previousClip.xy / previousClip.w * .5 + .5;
    outMotion = vec4(prevUv - currentUv, previousClip.w, 0);
    outViewZ = abs((g.view * vec4(worldPosition, 1)).z);
    outEmission = vec4(surface.emission, 0);
}
