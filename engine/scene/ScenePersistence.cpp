#include "ScenePersistence.h"
#include "core/World.h"
#include <algorithm>
#include <set>
namespace afterlight {
SceneDocument ScenePersistence::capture(const World& world, const AssetManager& assets) {
    world.storage_.changes.requireCommitted();
    SceneDocument s;
    const auto& r = world.resources;
    std::map<uint32_t, uint32_t> materialIndices;
    for (uint32_t i = 0; i < r.materials.size(); ++i)
        if (!r.transientMaterials.count(i)) {
            materialIndices[i] = uint32_t(s.materials.size());
            s.materials.push_back(MaterialDefinition::capture(r.materials[i], assets));
        }
    for (const auto& material : r.materialAssets)
        if (materialIndices.count(material.first))
            s.materialAssets.emplace(materialIndices.at(material.first),
                                     assets.resolve(material.second->reference()));
    s.camera = r.camera;
    s.navigation = r.navigation;
    for (const auto& script : r.scripts)
        s.scripts.push_back(assets.resolve(script));
    s.data = r.data;
    s.references = r.references;
    if (world.gameplay.playerId)
        s.player = world.get<Identity>(world.gameplay.playerId).persistentId;
    for (auto e : world.registry().entities()) {
        if (!world.persistent(e))
            continue;
        SceneEntity o;
        const auto& identity = world.get<Identity>(e);
        o.id = identity.persistentId;
        o.name = identity.name;
        o.enabled = !world.has<Disabled>(e);
        o.components = componentCatalog().capture(world, e, assets);
        if (auto render = o.components.find<SceneRender>()) {
            auto i = materialIndices.find(render->appearance.material);
            if (i == materialIndices.end())
                throw std::invalid_argument("Authored entity references a transient material");
            render->appearance.material = i->second;
        }
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
    world.gameplay.playerId = s.player.empty() ? 0 : world.findObject(s.player);
    world.gameplay.selected = world.gameplay.playerId;
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
        staged.gameplay.message = header.name;
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
    std::swap(world.gameplay, staged.gameplay);
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
uint32_t ScenePersistence::addMaterial(World& world, AssetManager& assets, const Json& value,
                                       bool persistent) {
    std::shared_ptr<const MaterialAsset> asset;
    Material material;
    if (value.contains("id")) {
        asset = assets.load<MaterialAsset>(AssetRef::fromJson(value));
        material = asset->parameters;
    } else
        material = MaterialDefinition::fromJson(value).resolve(assets);
    auto i = uint32_t(world.resources.materials.size());
    world.resources.materials.push_back(std::move(material));
    if (asset)
        world.resources.materialAssets.emplace(i, std::move(asset));
    if (!persistent)
        world.resources.transientMaterials.insert(i);
    world.resetHistory = true;
    return i;
}
static SceneResourceDescription resourceDocument(const World& world, const AssetManager& assets) {
    // Work in the live material index space, including temporary tool materials.
    SceneResourceDescription document;
    const auto& r = world.resources;
    for (const auto& material : r.materials)
        document.materials.push_back(MaterialDefinition::capture(material, assets));
    for (const auto& material : r.materialAssets)
        document.materialAssets.emplace(material.first, assets.resolve(material.second->reference()));
    document.camera = r.camera;
    document.navigation = r.navigation;
    document.scripts = r.scripts;
    document.references = r.references;
    document.data = r.data;
    document.player =
        world.gameplay.playerId ? world.get<Identity>(world.gameplay.playerId).persistentId : "";
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
    for (auto e : world.registry().view<Renderable>())
        if (world.get<Renderable>(e).appearance.material >= document.materials.size())
            throw std::invalid_argument("Live entity references a material outside the new table");
    if (!document.player.empty() && !world.findObject(document.player))
        throw std::invalid_argument("Scene player references a missing entity");
    for (const auto& ref : document.references)
        if (!world.findObject(ref.second))
            throw std::invalid_argument("Scene reference targets a missing entity: " + ref.first);
    auto& r = world.resources;
    auto next = resolveResources(document, assets);
    next.mapAsset = r.mapAsset;
    for (auto i : r.transientMaterials)
        if (i < next.materials.size())
            next.transientMaterials.insert(i);
    r = std::move(next);
    world.gameplay.playerId = document.player.empty() ? 0 : world.findObject(document.player);
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
