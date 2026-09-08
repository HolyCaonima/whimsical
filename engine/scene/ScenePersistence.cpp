#include "ScenePersistence.h"
#include "core/World.h"
#include <algorithm>
#include <set>
namespace afterlight {
SceneDocument ScenePersistence::capture(const World& world, const AssetManager& assets) {
    world.storage_.changes.requireCommitted();
    SceneDocument s;
    const auto& r = world.resources;
    s.camera = r.camera;
    s.navigation = r.navigation;
    for (const auto& script : r.scripts)
        s.scripts.push_back(assets.resolve(script));
    s.data = r.data;
    s.references = r.references;
    for (auto e : world.registry().entities()) {
        if (!world.persistent(e))
            continue;
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
uint32_t ScenePersistence::createEntity(World& world, const SceneEntity& o, AssetManager& assets,
                                        bool persistent) {
    auto batch = world.changes();
    auto e = world.create(o.name, o.id, persistent);
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
static SceneResources resolveResources(const SceneResourceDescription& s, AssetManager& assets) {
    SceneResources r;
    r.camera = s.camera;
    r.navigation = s.navigation;
    for (const auto& script : s.scripts) {
        (void)assets.load<ScriptAsset>(script);
        r.scripts.push_back(assets.resolve(script));
    }
    r.data = s.data;
    r.references = s.references;
    return r;
}
void ScenePersistence::instantiate(World& world, const SceneDocument& s, AssetManager& assets) {
    world.resources = resolveResources(s, assets);
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
    world.resetHistory = true;
}
void ScenePersistence::load(World& world, AssetManager& assets, const AssetPath& path) {
    const auto map = assets.load<SceneAsset>(path);
    restore(world, assets, map->scene, map->reference());
}
void ScenePersistence::restore(World& world, AssetManager& assets, const SceneDocument& document,
                               const AssetRef& source) {
    auto ref = source.id.empty() ? AssetRef{} : assets.resolve(source);
    AssetManager::Scope content(assets, ref.id.empty() ? ContentSourceRef{} : assets.origin(ref),
                                !ref.id.empty());
    document.validate();
    World staged;
    staged.storage_.registry.continueIdentitySequence(world.registry());
    staged.storage_.physics.continueHandleSequence(world.physics());
    instantiate(staged, document, assets);
    if (!ref.id.empty()) {
        auto header = assets.descriptor(ref.path);
        if (header.type != "Map")
            throw std::invalid_argument("Scene source must be a Map asset");
    }
    replace(world, staged, ref);
}
void ScenePersistence::replace(World& world, World& staged, const AssetRef& source) {
    auto oldEntities = world.registry().entities();
    world.storage_.changes.requireCommitted();
    auto batch = world.changes();
    // Transfer the validated scene once. Systems/subscriptions remain attached to
    // their World; only owned state and backend bindings cross this boundary.
    world.storage_.registry.exchangeScene(staged.storage_.registry);
    world.renderTargets.reset();
    world.storage_.physics.exchangeScene(staged.storage_.physics);
    world.storage_.renderScene.exchangeScene(staged.storage_.renderScene);
    world.storage_.jointColliders.exchangeBindings(staged.storage_.jointColliders);
    world.objectIds_.swap(staged.objectIds_);
    std::swap(world.resources, staged.resources);
    world.resources.mapAsset = source;
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
static SceneResourceDescription resourceDocument(const World& world, const AssetManager&) {
    SceneResourceDescription document;
    const auto& r = world.resources;
    document.camera = r.camera;
    document.navigation = r.navigation;
    document.scripts = r.scripts;
    document.references = r.references;
    document.data = r.data;
    return document;
}
Json ScenePersistence::resources(const World& world, const AssetManager& assets) {
    return resourceDocument(world, assets).json();
}
void ScenePersistence::setResources(World& world, AssetManager& assets, const Json& patch) {
    auto document = resourceDocument(world, assets);
    auto json = document.json();
    for (const auto& property : patch.members()) {
        if (!json.contains(property.first))
            throw std::invalid_argument("Unknown scene resource: " + property.first);
        json[property.first] = property.second;
    }
    document = SceneResourceDescription::fromJson(json);
    for (const auto& ref : document.references)
        if (!world.findObject(ref.second))
            throw std::invalid_argument("Scene reference targets a missing entity: " + ref.first);
    auto& r = world.resources;
    auto next = resolveResources(document, assets);
    next.mapAsset = r.mapAsset;
    r = std::move(next);
    world.resetHistory = true;
}
AssetRef ScenePersistence::save(World& world, AssetManager& assets, const AssetPath& path,
                                const std::string& name) {
    auto destination = assets.qualify(path);
    AssetManager::Scope content(assets, assets.origin(path));
    auto document = capture(world, assets);
    AssetHeader header;
    if (!world.resources.mapAsset.id.empty() && world.resources.mapAsset.source == assets.origin(path)->id &&
        assets.resolve(world.resources.mapAsset).path == destination)
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
