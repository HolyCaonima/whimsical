// Shared by every surface raster output: interpolation, material evaluation and alpha clipping.
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec4 previousClip;
layout(location = 3) flat in uint instanceIndex;
layout(location = 4) in vec3 vertexColor;
layout(location = 5) in vec2 texcoord;
layout(location = 6) in vec4 worldTangent;
SurfaceData rasterSurface(Instance i) {
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
    return surface;
}
