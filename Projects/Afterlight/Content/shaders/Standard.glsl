SurfaceData s = DefaultSurface(ctx);
vec2 uv = ctx.uv * properties.uvScale;
vec4 base = SampleTexture(ctx, textures.baseColor, uv);
vec3 orm = SampleTexture(ctx, textures.orm, uv).rgb;
s.albedo = properties.baseColor * ctx.vertexColor * base.rgb;
s.opacity = base.a;
s.roughness = properties.roughness * orm.g;
s.metallic = properties.metallic * orm.b;
s.emission = properties.emission;
if (textures.normal >= 0) {
    vec3 normal = SampleTexture(ctx, textures.normal, uv).xyz * 2 - 1;
    normal.xy *= properties.normalStrength;
    s.normal = TangentNormal(ctx, normal);
}
return s;
