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
// A leaf silhouette illustrates custom masked semantics without renderer flags.
vec2 leaf = ctx.uv * 2 - 1;
s.opacity *= 1 - smoothstep(.88,1.0, length(leaf));
s.albedo *= mix(.78,1.0,1-abs(leaf.x));
return s;
