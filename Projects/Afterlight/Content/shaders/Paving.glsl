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
// Staggered paving belongs to this Shader, including at secondary ray hits.
if (ctx.normal.y > .95 && ctx.position.y < .08) {
    vec2 tile = ctx.position.xz;
    tile.x += mod(floor(tile.y), 2) * .5;
    vec2 f = fract(tile);
    vec2 edge = smoothstep(vec2(.014), vec2(.014 + max(ctx.footprint,.0001)), min(f,1-f));
    s.albedo *= mix(.32,1,min(edge.x,edge.y));
    s.albedo *= .95 + .05 * sin(ctx.position.x*.7) * cos(ctx.position.z*.53);
}
return s;
