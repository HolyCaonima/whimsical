SurfaceData s = DefaultSurface(ctx);
s.albedo = properties.baseColor * ctx.vertexColor;
s.roughness = properties.roughness;
s.metallic = properties.metallic;
s.emission = properties.emission + s.albedo * 0.12;
return s;
