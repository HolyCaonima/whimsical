# Shader → Material

`ShaderAsset` owns GLSL source, an ordered property/texture schema, the material model,
and render state. `MaterialAsset` owns a resolved `Material`: a shared immutable Shader,
schema-ordered values and shared Texture assets. `MaterialDefinition` is the asset payload;
Render components and Maps reference Material assets by durable ID.
Neither assets, World nor Frame contain texture descriptors or Shader dispatch IDs.

## Compilation model

The Vulkan renderer uses ray queries inside compute shaders, not a ray-tracing pipeline
with an SBT. `ShaderCompiler` therefore uses GLSL source linking:

* Each Shader gets one GBuffer fragment program and graphics pipeline. The common vertex
  program already handles static and skinned geometry; there is no skinning permutation.
* Each active Shader generation is linked once into a generated dispatch table in
  the six `ShaderCompiler::surfacePasses` (lighting, GI reuse, DI temporal/spatial,
  resolve and gradient replay). The table is keyed by a renderer-owned
  Shader index, not a Material index. The compute pipeline cache is keyed by the ordered
  set of immutable Shader generations. Adding another instance does not relink it.
* Composite, gradient filtering/confidence, UI and the GBuffer vertex program remain build-time programs; they do not
  evaluate authored surfaces. Lighting/BRDF/ReSTIR/NRD remain renderer responsibilities.

This avoids changing the current query architecture or adding callable shader machinery.
There is no Material compilation and no feature/permutation registry. The only surface
variants are the raster and ray adapters, with six ray-consuming pass entry points.

Runtime glslang compiles source with Vulkan 1.2 semantics. Its process is launched directly,
without a shell or visible console. The SPIR-V cache uses the complete expanded source
and pass as its key, including the surface contract, pass template and recursively expanded pinned RTXDI headers. Compiler diagnostics
map numbered GLSL source strings to Shader asset paths. Generated inputs, SPIR-V and logs
are retained under `build/shaders/runtime-<id>` for inspection; cache reuse is in-process.
The current development runtime needs the source tree and bundled glslang. Offline cooking,
persistent binary caching and eviction are future work, not an implied shipping pipeline.

## Surface contract

`engine/render/shaders/surface.glsl` is the public ABI:

```glsl
SurfaceData EvaluateSurface(MaterialContext ctx);
```

The asset payload is the **body** of that function. The generator supplies local typed
`properties` and `textures` structs from the schema. Local names are scoped inside each
generated function, so different Shader sources can be linked without symbol collisions.
Payloads are self-contained GLSL function bodies; there is no include/module dependency
system or free-standing helper-function linker yet. Keep surface source independent of
pass macros, engine buffers and derivatives; use the contract helpers instead.

`MaterialContext` provides material identity, world position, normalized world normal,
world tangent and handedness, vertex color, raw UV, view direction, front-facing state,
explicit texture LOD and a world-space filtering footprint. Both adapters orient normals
toward the visible side for two-sided rendering. Shader code returns linear albedo,
world normal, emission, roughness, metallic and opacity in `SurfaceData`.

`SampleTexture(ctx, handle, uv)` returns white for an unbound texture. Raster uses texture
gradients; ray queries use explicit LOD 2. Raster footprint comes from world-position
derivatives, while secondary rays use distance / image height with a small lower bound.
These are explicit sampling approximations, not different surface equations. Full ray
differentials/cones are not implemented. `TangentNormal` converts a tangent-space normal;
normal-map fallback belongs in the Shader source, where the texture handle is known.

GBuffer encoding and committed ray-hit shading call the generated evaluation. Candidate
acceptance in **both** GI/specular traces and shadow visibility uses the same opacity and
cull rules. BLAS triangles are non-opaque so rejected masked candidates can continue to
geometry behind them. Opaque two-sided Shader instances use the renderer-owned TLAS
force-opaque flag to bypass candidate evaluation. Material changes reset history and
refresh instance attributes before the TLAS update, including changes to that flag. Opaque shaders ignore opacity. Roughness has the same .08 floor in
the GBuffer and reconstructed ray surface.

Supported metadata is deliberately narrow: `metallicRoughness`, `opaque` or `masked`,
`none`/`back`/`front` culling and an alpha cutoff. Deferred surfaces always depth-test and
write depth. Unsupported models and transparent blending are rejected at asset loading,
rather than silently rendered as PBR. A new material model would also need a lighting and
GBuffer contract; it is not an arbitrary string that changes only the fragment stage.

## Authoring

The project contains `shaders/Standard.asset`, `Paving.asset` and `Foliage.asset`, with
external GLSL payloads. Paving owns its seam/stain equations. Foliage demonstrates a
procedural masked silhouette. Neither name appears in generic renderer code.

The Shader header's `metadata` contains:

```json
{
  "materialModel": "metallicRoughness",
  "properties": [
    {"name": "tint", "type": "vec3", "default": [0.3, 0.7, 0.2]},
    {"name": "roughness", "type": "float", "default": 0.7}
  ],
  "textures": ["color"],
  "renderState": {"surface": "masked", "cull": "none", "alphaCutoff": 0.5}
}
```

Its GLSL payload can be:

```glsl
SurfaceData s = DefaultSurface(ctx);
vec4 color = SampleTexture(ctx, textures.color, ctx.uv);
s.albedo = properties.tint * ctx.vertexColor * color.rgb;
s.opacity = color.a;
s.roughness = properties.roughness;
return s;
```

Material payloads contain `shader: AssetRef`, `properties: {name: value}` and
`textures: {name: AssetRef}`. Properties may be float, vec2, vec3 or vec4. Omitted values
use Shader defaults; omitted textures remain unbound. Unknown names, wrong dimensions,
duplicate schema names and unsupported metadata fail at resolution. Values use float
precision on document load so save/capture round trips are stable.

`MaterialBindings` assigns the runtime Shader table, deduplicates Texture assets, and packs
176-byte GPU records (Shader index, eight vec4 property slots, eight texture handles).
The schema uses at most eight properties and eight textures per Shader. This fixed ABI
keeps the existing descriptor layout small; it is a capacity limit, not eight hardcoded
material features. Global limits remain 256 Materials and 64 distinct textures.

Material edits compare CPU instances and update bindings after the previous GPU fence.
Source/schema/state reloads produce new immutable Shader generations through the existing
AssetManager cache/scan path. Existing Frames retain their generations; map reload brings
the new generation into the renderer. Source/schema/set changes select new programs and
invalidate history. Ordinary values/textures invalidate history without Shader compilation.

## Persistence and migration

Map payloads use version 9; the outer ALAS1 envelope stays version 1. Each
`render.material` is an AssetRef, just like `mesh`. Runtime components retain immutable
Material assets, while RenderScene owns the dense GPU material table and remaps only
render proxies when an unused slot is reclaimed. Map material tables and the parallel
`materialAssets` index map have been removed.

Run `python tools/migrate_material_assets.py <Map.asset>` on v7/v8 maps. Existing
Material asset bindings retain their IDs; inline definitions become standalone assets.
Project-owned numeric aliases (for example RainCourt's `data.materials`) must also be
converted to AssetRefs by the project. Loads never create assets. New materials are
created through the regular Content asset API; `Engine.material(...)` and
`Engine.scene.addMaterial(...)` have been removed. `Engine.setMaterial(entity, ref)`
accepts an AssetRef or asset path. Shader/Texture reference and property semantics are
unchanged. See [the framework audit](material-asset-references.md).

The following validation descriptions predate v9. Their fixtures have not been changed
or run as part of this migration, as requested.

`shader_tests` covers defaults, invalid schemas, reference/instance round trips, renderer
binding deduplication, Shader-ID stability on Material reorder, real SPIR-V compilation
of all three example Shaders and an unrelated custom schema, per-pass cache reuse, source
invalidation and compiler diagnostics. Vulkan audit reports also expose Shader count,
surface compilation count and cached raster pipeline count.

`python tools/test_shader_surfaces.py` runs an optional Vulkan regression on a generated
three-Shader project. It verifies that a fully discarded surface matches an absent one
exactly in albedo, direct lighting and resolved GI/specular, that visible foliage changes
all three, and that 80-frame runs compile only six surface programs with zero validation
errors. It requires a ray-query-capable GPU and leaves its projects/captures for review.
