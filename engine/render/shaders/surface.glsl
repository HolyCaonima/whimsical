// Surface ABI shared by raster and ray-query adapters. Shader sources only see
// this context, their schema and the sampling helpers; no pass-specific branches.
struct MaterialContext {
    uint material;
    vec3 position;
    vec3 normal;
    vec4 tangent;
    vec3 vertexColor;
    vec2 uv;
    vec3 viewDirection;
    bool frontFacing;
    float textureLod;
    float footprint; // World-space pixel/cone width, for procedural filtering.
};
struct SurfaceData {
    vec3 albedo;
    vec3 normal;
    vec3 emission;
    float roughness;
    float metallic;
    float opacity;
};
SurfaceData DefaultSurface(MaterialContext ctx) {
    return SurfaceData(vec3(1), normalize(ctx.normal), vec3(0), .5, 0, 1);
}
vec4 SampleTexture(MaterialContext ctx, int handle, vec2 uv) {
    if (handle < 0) return vec4(1);
#ifdef FRAGMENT_PASS
    return textureGrad(materialTextures[nonuniformEXT(handle)], uv, dFdx(uv), dFdy(uv));
#else
    return textureLod(materialTextures[nonuniformEXT(handle)], uv, ctx.textureLod);
#endif
}
vec3 TangentNormal(MaterialContext ctx, vec3 normal) {
    vec3 n = normalize(ctx.normal);
    vec3 t = normalize(ctx.tangent.xyz - n * dot(n, ctx.tangent.xyz));
    return normalize(mat3(t, cross(n,t) * ctx.tangent.w, n) * normal);
}
