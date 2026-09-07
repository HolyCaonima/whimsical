#include "ScenePersistence.h"
#include "core/World.h"
namespace afterlight {
SceneDocument ScenePersistence::capture(const World& world, const AssetManager& assets) {
    SceneDocument s;
    const auto& r = world.resources;
    for (const auto& material : r.materials)
        s.materials.push_back(MaterialDefinition::capture(material, assets));
    for (const auto& material : r.materialAssets)
        s.materialAssets.emplace(material.first, assets.resolve(material.second->reference()));
    s.lights = r.lights;
    s.camera = r.camera;
    s.navigation = r.navigation;
    for (const auto& script : r.scripts)
        s.scripts.push_back(assets.resolve(script));
    s.data = r.data;
    s.references = r.references;
    if (world.gameplay.playerId)
        s.player = world.get<Identity>(world.gameplay.playerId).persistentId;
    for (auto e : world.registry().entities()) {
        SceneEntity o;
        const auto& identity = world.get<Identity>(e);
        o.id = identity.persistentId;
        o.name = identity.name;
        o.enabled = !world.has<Disabled>(e);
        o.interactable = world.has<Interactable>(e);
        if (auto t = world.registry().tryGet<Transform>(e))
            o.transform =
                SceneTransform{t->local, t->parent ? world.get<Identity>(t->parent).persistentId : ""};
        if (auto render = world.registry().tryGet<Renderable>(e)) {
            o.render = SceneRender{render->appearance};
            if (render->mesh)
                o.render->mesh = assets.resolve(render->mesh->reference());
        }
        if (auto c = world.registry().tryGet<Collider>(e)) {
            const auto& b = world.physics().body(c->body);
            o.collider = SceneCollider{b.shape, b.motion, b.layer, b.blocking, b.walkable, b.pickable};
        }
        if (auto a = world.registry().tryGet<Animator>(e)) {
            if (!a->asset)
                throw std::invalid_argument("Cannot save an unregistered animation solver: " + identity.name);
            o.animation = SceneAnimation{assets.resolve(*a->asset), a->applyRootMotion, a->rootOffset,
                                         a->instance->attributes()};
        } else if (auto j = world.registry().tryGet<JointPose>(e))
            o.joints = j->model;
        if (auto skin = world.registry().tryGet<Skin>(e))
            o.skin = assets.resolve(skin->mesh->reference());
        if (auto data = world.registry().tryGet<ScriptData>(e))
            o.data = data->value;
        o.jointColliders = world.storage_.jointColliders.describe(e);
        s.entities.push_back(std::move(o));
    }
    s.validate();
    return s;
}
void ScenePersistence::addComponents(World& world, uint32_t e, const SceneEntity& o, AssetManager& assets) {
    if (o.data)
        world.add<ScriptData>(e, ScriptData{*o.data});
    if (o.transform) {
        Entity parent = 0;
        if (!o.transform->parent.empty()) {
            parent = world.findObject(o.transform->parent);
            if (!parent)
                throw std::invalid_argument("Missing parent entity");
            world.get<Transform>(parent);
        }
        world.transforms.add(e, o.transform->local);
        if (parent)
            world.transforms.setParent(e, parent, false);
    }
    if (o.interactable)
        world.add<Interactable>(e);
    if (o.render) {
        if (o.render->appearance.material >= world.resources.materials.size())
            throw std::invalid_argument("Invalid material");
        auto mesh = o.render->mesh ? assets.load<StaticMesh>(*o.render->mesh) : nullptr;
        world.render.add(e, o.render->appearance, std::move(mesh));
    }
    if (o.collider) {
        const auto& c = *o.collider;
        PhysicsBody b;
        b.shape = c.shape;
        b.motion = c.motion;
        b.layer = c.layer;
        b.blocking = c.blocking;
        b.walkable = c.walkable;
        b.pickable = c.pickable;
        world.motion.add(e, b);
    }
    if (o.animation) {
        const auto& a = *o.animation;
        auto asset = assets.load<animation::Asset>(a.asset);
        if (world.has<Animator>(e))
            throw std::logic_error("Animator already present");
        world.animation.attachAnimation(e, *asset, a.rootMotion, a.rootOffset, a.attributes);
    } else if (o.joints)
        world.animation.setAnimationJoints(e, *o.joints);
    if (o.skin)
        world.animation.setSkinnedMesh(e, assets.load<SkinnedMesh>(*o.skin));
    for (const auto& c : o.jointColliders)
        world.animation.addAnimationCollider(e, c.joint, c.shape, c.local, c.blocking);
}
uint32_t ScenePersistence::createEntity(World& world, const SceneEntity& o, AssetManager& assets) {
    auto e = world.create(o.name, o.id);
    try {
        addComponents(world, e, o, assets);
        world.setEnabled(e, o.enabled);
    } catch (...) {
        world.destroy(e);
        throw;
    }
    return e;
}
void ScenePersistence::instantiate(World& world, const SceneDocument& s, AssetManager& assets) {
    auto& r = world.resources;
    for (uint32_t i = 0; i < s.materials.size(); ++i) {
        auto binding = s.materialAssets.find(i);
        if (binding == s.materialAssets.end())
            r.materials.push_back(s.materials[i].resolve(assets));
        else {
            auto material = assets.load<MaterialAsset>(binding->second);
            r.materialAssets.emplace(i, material);
            r.materials.push_back(material->parameters);
        }
    }
    r.lights = s.lights;
    r.camera = s.camera;
    r.navigation = s.navigation;
    for (const auto& script : s.scripts) {
        (void)assets.load<ScriptAsset>(script);
        r.scripts.push_back(assets.resolve(script));
    }
    r.data = s.data;
    r.references = s.references;
    // Identity and hierarchy are resolved before any spatial backend or solver binds.
    for (const auto& o : s.entities) {
        auto e = world.create(o.name, o.id);
        if (o.transform)
            world.transforms.add(e, o.transform->local);
    }
    for (const auto& o : s.entities)
        if (o.transform && !o.transform->parent.empty())
            world.transforms.setParent(world.findObject(o.id), world.findObject(o.transform->parent), false);
    for (auto o : s.entities) {
        o.transform.reset();
        auto e = world.findObject(o.id);
        addComponents(world, e, o, assets);
        world.setEnabled(e, o.enabled);
    }
    world.gameplay.playerId = s.player.empty() ? 0 : world.findObject(s.player);
    world.gameplay.selected = world.gameplay.playerId;
    world.resetHistory = true;
}
void ScenePersistence::load(World& world, AssetManager& assets, const AssetPath& path) {
    const auto map = assets.load<SceneAsset>(path);
    World staged;
    instantiate(staged, map->scene, assets);
    world.clearScene();
    instantiate(world, map->scene, assets);
    world.resources.mapAsset = map->reference();
    world.gameplay.message = map->header().name;
}
AssetRef ScenePersistence::save(World& world, AssetManager& assets, const AssetPath& path,
                                const std::string& name) {
    auto document = capture(world, assets);
    AssetHeader header;
    if (!world.resources.mapAsset.id.empty() && assets.resolve(world.resources.mapAsset).path == path)
        header = assets.descriptor(path);
    else {
        header.id = newPersistentId();
        header.type = "Map";
    }
    header.name = name;
    header.storage = PayloadStorage::Inline;
    header.source.clear();
    auto ref = assets.save(path, header, document.json().dump());
    world.resources.mapAsset = ref;
    return ref;
}
} // namespace afterlight
