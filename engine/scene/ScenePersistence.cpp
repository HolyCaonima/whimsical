#include "ScenePersistence.h"
#include "core/World.h"

namespace afterlight {
SceneDocument ScenePersistence::capture(const World& world, const AssetManager& assets) {
    SceneDocument s;
    s.materials = world.materials;
    for (const auto& material : world.materialAssets)
        s.materialAssets.emplace(material.first, assets.resolve(material.second->reference()));
    s.lights = world.lights;
    s.camera = world.camera;
    s.navigation = world.navigation;
    for (const auto& script : world.sceneScripts)
        s.scripts.push_back(assets.resolve(script));
    s.data = world.sceneData;
    s.references = world.sceneReferences;
    if (world.playerId)
        s.player = world.entity(world.playerId).persistentId;
    for (const auto& object : world.objects_) {
        if (!object.alive)
            continue;
        SceneObject o;
        o.id = object.persistentId;
        o.name = object.name;
        o.position = object.position;
        o.yaw = object.yaw;
        o.render = object.render;
        if (object.staticMesh)
            o.staticMesh = assets.resolve(object.staticMesh->reference());
        o.enabled = object.enabled;
        o.interactable = object.interactable;
        const auto& body = world.physics_.body(object.physical);
        o.collider = body.shape;
        o.motion = body.motion;
        o.layer = body.layer;
        o.blocking = body.blocking;
        o.walkable = body.walkable;
        o.pickable = body.pickable;
        auto animation = world.animations_.find(object.id);
        if (animation != world.animations_.end()) {
            const auto& a = animation->second;
            if (!a.asset)
                throw std::invalid_argument("Cannot save an unregistered animation solver: " + object.name);
            SceneAnimation sa;
            sa.asset = assets.resolve(*a.asset);
            sa.rootMotion = a.applyRootMotion;
            sa.rootOffset = a.rootOffset;
            sa.attributes = a.instance->attributes();
            if (a.mesh) {
                if (a.mesh->header().id.empty())
                    throw std::invalid_argument("Cannot save an unregistered mesh");
                sa.mesh = assets.resolve(a.mesh->reference());
            }
            o.animation = std::move(sa);
        } else
            o.joints = object.joints;
        o.jointColliders = world.animationCollision_.describe(object.id);
        s.objects.push_back(std::move(o));
    }
    s.validate();
    return s;
}
void ScenePersistence::instantiate(World& world, const SceneDocument& s, AssetManager& assets) {
    world.materials = s.materials;
    for (const auto& binding : s.materialAssets) {
        auto material = assets.load<MaterialAsset>(binding.second);
        world.materialAssets.emplace(binding.first, material);
        auto& parameters = world.materials[binding.first];
        parameters = material->parameters;
        for (int channel = 0; channel < 3; ++channel) {
            auto texture = material->textures[channel];
            if (!texture)
                continue;
            auto found = std::find(world.textures.begin(), world.textures.end(), texture);
            parameters.textures[channel] = int(found - world.textures.begin());
            if (found == world.textures.end())
                world.textures.push_back(texture);
        }
    }
    world.lights = s.lights;
    world.camera = s.camera;
    world.navigation = s.navigation;
    for (const auto& script : s.scripts) {
        (void)assets.load<ScriptAsset>(script);
        world.sceneScripts.push_back(assets.resolve(script));
    }
    world.sceneData = s.data;
    world.sceneReferences = s.references;
    for (const auto& o : s.objects) {
        auto id = world.spawn(o.name, o.render.shape, o.position, o.render.scale, o.render.material,
                              o.blocking, o.interactable, o.id);
        auto& object = world.mutableObject(id);
        object.yaw = o.yaw;
        object.render = o.render;
        if (o.staticMesh)
            world.setStaticMesh(id, assets.load<StaticMesh>(*o.staticMesh));
        world.setColliderShape(id, o.collider);
        world.configureCollider(id, o.layer, o.blocking, o.walkable, o.pickable);
        world.physics_.setMotion(object.physical, o.motion);
        if (o.animation) {
            const auto& a = *o.animation;
            auto asset = assets.load<animation::Asset>(a.asset);
            world.attachAnimation(id, *asset, a.rootMotion, a.rootOffset);
            for (const auto& v : a.attributes)
                world.setAnimationAttribute(id, v.first, v.second);
            if (a.mesh)
                world.setSkinnedMesh(id, assets.load<SkinnedMesh>(*a.mesh));
            // Initialize joint bindings from the rest pose without advancing inference.
            auto model = asset->skeleton()->toModel(asset->skeleton()->restPose());
            for (const auto& p : model)
                object.joints.push_back({p.position + a.rootOffset, p.rotation});
        } else
            object.joints = o.joints;
        for (const auto& c : o.jointColliders)
            world.addAnimationCollider(id, c.joint, c.shape, c.local, c.blocking);
        world.syncPose(object);
        world.setEnabled(id, o.enabled);
        world.publishAttributes(object);
    }
    world.playerId = s.player.empty() ? 0 : world.findObject(s.player);
    world.selected = world.playerId;
    world.resetHistory = true;
}
void ScenePersistence::load(World& world, AssetManager& assets, const AssetPath& path) {
    const auto map = assets.load<SceneAsset>(path);
    // Preflight every dependency, enum, skeleton binding and physical shape before
    // touching the live World. Runtime playback never enters the saved document.
    World staged;
    instantiate(staged, map->scene, assets);
    world.clearScene();
    instantiate(world, map->scene, assets);
    world.mapAsset_ = map->reference();
    world.message = map->header().name;
}
AssetRef ScenePersistence::save(World& world, AssetManager& assets, const AssetPath& path,
                                const std::string& name) {
    auto document = capture(world, assets);
    AssetHeader header;
    if (!world.mapAsset_.id.empty() && assets.resolve(world.mapAsset_).path == path)
        header = assets.descriptor(path);
    else {
        header.id = newPersistentId();
        header.type = "Map";
    }
    header.name = name;
    // Scene authoring always writes the document inline, even if an imported Map used external storage.
    header.storage = PayloadStorage::Inline;
    header.source.clear();
    auto ref = assets.save(path, header, document.json().dump());
    world.mapAsset_ = ref;
    return ref;
}
} // namespace afterlight
