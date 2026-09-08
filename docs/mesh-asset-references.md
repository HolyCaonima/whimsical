# Render geometry bindings

Render has an optional StaticMesh asset reference and a Material asset reference.
Skin binds a SkinnedMesh and its joint deformation; static and skinned bindings are
mutually exclusive. A Render without either binding draws nothing. Removing a
binding never substitutes another mesh.

Box and Capsule are ordinary engine StaticMesh assets at `/Engine/Meshes/Box` and
`/Engine/Meshes/Capsule`. They use the same asset cache, geometry allocation and
release path as imported meshes. The renderer has no primitive enum, reserved
primitive slots or procedural fallback. Rebuild the asset payloads with
`python tools/generate_primitive_meshes.py`.

The host declares `/Engine` a shared Content mount. Dependency scopes accept local
assets and explicitly shared mounts; unrelated project mounts remain isolated.
Saving local references uses `/Game`, while shared references keep their stable
mount alias (such as `/Engine`) and omit the transient source identity. Applications
embedding AssetManager should mount their shared libraries with `shared = true`.

Map v10 stores explicit mesh references. Run
`python tools/migrate_mesh_assets.py <Map.asset>` on v9 maps. The migration preserves
existing static and skinned bindings, replaces unbound primitive shapes with engine
mesh references, and removes render.shape. Older maps first need the material
asset migration. Colliders retain their independent physics shapes.

The editor's Cube/Capsule creation commands bind these assets explicitly. Mesh
fields use the same searchable asset picker as Material fields, including engine
assets. Geometry-free Render components can be bound through their JSON editor.
