#include "ScenePersistence.h"
#include "core/World.h"
#include <algorithm>
#include <set>
namespace afterlight {
SceneDocument ScenePersistence::capture(const World& world, const AssetManager& assets) {
    world.storage_.changes.requireCommitted();
    SceneDocument s;
    const auto& r = world.resources;
    for (const auto& material : r.materials)
        s.materials.push_back(MaterialDefinition::capture(material, assets));
    for (const auto& material : r.materialAssets)
        s.materialAssets.emplace(material.first, assets.resolve(material.second->reference()));
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
        o.components = componentCatalog().capture(world, e, assets);
        s.entities.push_back(std::move(o));
    }
    s.validate();
    return s;
}
void ScenePersistence::addComponents(World& world, uint32_t e, const SceneEntity& o, AssetManager& assets) {
    componentCatalog().attach(world, e, o.components, assets);
}
uint32_t ScenePersistence::createEntity(World& world, const SceneEntity& o, AssetManager& assets) {
    auto batch = world.changes();
    auto e = world.create(o.name, o.id);
    try {
        addComponents(world, e, o, assets);
        world.setEnabled(e, o.enabled);
    } catch (...) {
        world.destroy(e);
        world.storage_.changes.forget(e);
        batch.commit();
        throw;
    }
    batch.commit();
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
    r.camera = s.camera;
    r.navigation = s.navigation;
    for (const auto& script : s.scripts) {
        (void)assets.load<ScriptAsset>(script);
        r.scripts.push_back(assets.resolve(script));
    }
    r.data = s.data;
    r.references = s.references;
    // Create all identities, then instantiate parents before children. Component
    // ordering within each entity comes entirely from the catalog.
    for (const auto& o : s.entities)
        world.create(o.name, o.id);
    std::set<std::string> installed;
    std::function<void(const SceneEntity&)> install = [&](const SceneEntity& o) {
        if (installed.count(o.id))
            return;
        if (auto t = o.components.find<SceneTransform>(); t && !t->parent.empty()) {
            auto parent = std::find_if(s.entities.begin(), s.entities.end(),
                                       [&](const auto& candidate) { return candidate.id == t->parent; });
            install(*parent);
        }
        auto e = world.findObject(o.id);
        addComponents(world, e, o, assets);
        world.setEnabled(e, o.enabled);
        installed.insert(o.id);
    };
    for (const auto& o : s.entities)
        install(o);
    world.gameplay.playerId = s.player.empty() ? 0 : world.findObject(s.player);
    world.gameplay.selected = world.gameplay.playerId;
    world.resetHistory = true;
}
void ScenePersistence::load(World& world, AssetManager& assets, const AssetPath& path) {
    const auto map = assets.load<SceneAsset>(path);
    World staged;
    staged.storage_.registry.continueIdentitySequence(world.registry());
    staged.storage_.physics.continueHandleSequence(world.physics());
    instantiate(staged, map->scene, assets);
    auto oldEntities = world.registry().entities();
    world.storage_.changes.requireCommitted();
    auto batch = world.changes();
    // Transfer the validated scene once. Systems/subscriptions remain attached to
    // their World; only owned state and backend bindings cross this boundary.
    world.storage_.registry.exchangeScene(staged.storage_.registry);
    world.storage_.physics.exchangeScene(staged.storage_.physics);
    world.storage_.renderScene.exchangeScene(staged.storage_.renderScene);
    world.storage_.jointColliders.exchangeBindings(staged.storage_.jointColliders);
    world.objectIds_.swap(staged.objectIds_);
    std::swap(world.resources, staged.resources);
    std::swap(world.gameplay, staged.gameplay);
    world.resources.mapAsset = map->reference();
    world.gameplay.message = map->header().name;
    world.resetHistory = true;
    for (auto e : oldEntities) {
        world.storage_.changes.mark<Identity>(e);
        for (const auto& [name, c] : componentCatalog().entries())
            if (c.present(staged.registry(), e))
                world.storage_.changes.mark(e, c.runtimeType);
    }
    for (auto e : world.registry().entities()) {
        world.storage_.changes.mark<Identity>(e);
        for (const auto& [name, c] : componentCatalog().entries())
            if (c.present(world.registry(), e))
                world.storage_.changes.mark(e, c.runtimeType);
    }
    try {
        batch.commit();
    } catch (const std::exception& error) {
        throw CommittedError(std::string("Scene committed; publication failed: ") + error.what());
    }
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
