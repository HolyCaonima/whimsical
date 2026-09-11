# Material rendering policy

Rendering behavior is authored on a Material, evaluated by the render system, and
executed by the GPU backend. Entity purpose is not part of that contract.

## Data ownership

| Owner | Data and responsibility |
| --- | --- |
| Shader asset | Surface function, property/texture schema, material model. No pipeline, layer, depth or ray policy. |
| Material asset | Shader reference, property/texture values, `renderState`. Immutable once resolved. |
| Render component / proxy | Material binding, visibility and instance shadow visibility. Transform and geometry retain their existing owners. |
| MaterialBindings | Dense GPU material/texture/shader indices, packed acceptance data, resolved ray visibility/opacity policies. No authored GPU indices. |
| RenderPipeline | Program cache, visible draw groups, output adapters, ordering and pass declarations. |
| GpuScene | Geometry/instance storage, acceleration structures and execution of prepared draw lists. No entity classification or shader override path. |
| RenderGraph | Attachment storage, clear/load operations, resource dependencies and synchronization. |
| Application / host | Persistence, selection and outline requests. Rendering policy does not imply editor behavior. |

`overlay` and `overlayColor` have been removed from components, snapshots, serialization
and the instance ABI. A vertex tint is an ordinary shader property, not an instance
field with a second material implementation behind it.

## Material declaration

`renderState` is optional on a Material payload. Its complete default is:

```json
{
  "domain": "surface",
  "surface": "opaque",
  "cull": "none",
  "depthTest": true,
  "depthWrite": true,
  "depthCompare": "less",
  "blend": "opaque",
  "alphaCutoff": 0.5,
  "rayVisible": true,
  "layer": 0
}
```

The `surface` domain writes the deferred G-buffer. It requires layer 0 and opaque
blending: blending independent G-buffer attributes is not a transparency algorithm.
The `display` domain writes `albedo + emission` and opacity after scene tone mapping,
without lighting. Its color values are display-space values. It supports `opaque`,
`alpha` (source alpha / one minus source alpha) and `additive` (source alpha / one)
blending. This domain is an output contract, not a name for editor entities.

Both domains use the same surface function and acceptance rule. `surface: "masked"`
clips opacity below `alphaCutoff`; `opaque` does not clip. Culling accepts `none`,
`back` and `front`. The same material data controls raster and ray-hit acceptance.

Depth test and write are independent declarations. The Vulkan adapter implements
write-without-test using an always-passing enabled test, because Vulkan gates depth
writes on the depth test. Comparisons are `never`, `less`, `equal`, `lessEqual`,
`greater`, `notEqual`, `greaterEqual` and `always`.

## Layers and outputs

Layer 0 shares scene depth. Display layer 0 loads the depth produced by surface
rasterization. Positive display layers execute in ascending order; each clears its
own depth to 1 and preserves scene depth. Draws within the same layer share depth.
This expresses geometry that stays in front of the scene while retaining internal
occlusion, without disabling depth or introducing a gizmo branch.

One classification produces both color and EntityID draw lists. They share visibility,
layer order, culling, clipping and depth policy. Opaque draws come first; blended draws
are ordered back-to-front by instance origin. This is object sorting, not per-triangle
transparency. Integer EntityID outputs never blend; picking returns the last fragment
that passes the same coverage/depth rules. For opacity-based picking holes, author
masked coverage explicitly.

Scheduling encodes that order in a `uint32_t` sort key. The raster backend uses
stable ascending sorting, then batches only adjacent draws with the same pipeline
and geometry. Different material values can share a batch; the ordered instance
slot stream preserves their individual identity and blending order. See
[raster instancing](instanced-rendering.md) for the interface and lifetime contract.

The graph's depth declaration distinguishes an explicit clear value from `nullopt`
(load existing depth). Clear declares an overwrite; load declares a modification.
Only the graph chooses attachment load/store operations and barriers. Layer scratch
depth is a transient rendering resource, never an asset reference or component field.

## Programs, updates and ray visibility

Raster pipeline identity consists of Shader generation, domain and fixed-function
state. Layer, ray visibility, alpha cutoff and material values do not create shader
permutations. EntityID programs are prepared when an ID output is requested.

Material acceptance is runtime GPU data: the material header packs shader index,
surface/cull flags and alpha cutoff. Both raster and ray adapters use that header,
so two Materials can share a Shader while using different clipping/culling policies.

`rayVisible: false` removes an instance from all ray queries. Otherwise instance
`castShadow` controls the shadow bit while other ray bits remain visible. These are
independent of the output domain. The implementation retains the existing stable
one-slot-per-proxy TLAS and sets the mask to zero; this is logical exclusion, not
physical TLAS compaction or BLAS allocation pruning.

Material binding updates upload material data. Only changes to the resolved ray policy
refresh instance masks/opaque flags, even when no instance delta was published. Ordinary
property updates do not rewrite instance attributes; transforms retain their separate
fast path. Material data, texture and shader changes do **not** reset lighting history;
GPU upload lifetime is not temporal-validity policy. Existing temporal algorithms
decide sample reuse. Explicit scene/history resets remain separate operations.

## Resource migration

Move the old Shader metadata `renderState` into every Material that references it.
Shaders now reject that obsolete field so nondefault masked/cull settings cannot be
silently lost. Default opaque/two-sided state can be omitted on Materials. Loading
never rewrites resources. The repository's Shader assets and resource generators
have been migrated together.

HumanEditor uses a project Shader with `baseColor * ctx.vertexColor`, ordinary axis
and highlight Material assets, and `domain: "display", layer: 1, rayVisible: false`.
Its components only bind meshes/materials. It still owns transient lifetime and
selection; none of those rules appear in the renderer.
