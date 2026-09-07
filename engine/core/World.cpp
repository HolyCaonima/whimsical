#include "World.h"
#include "CpuProfile.h"
#include "debug/PhysicsDebug.h"
namespace afterlight {
World::World() {
    storage_.removeComponent = [this](Entity e, std::type_index type) {
        auto c = componentCatalog().find(type);
        if (!c)
            throw std::logic_error("Unregistered managed component");
        componentCatalog().remove(*this, e, c->name);
    };
}
Entity World::create(std::string name, std::string persistentId) {
    if (persistentId.empty())
        persistentId = newPersistentId();
    validatePersistentId(persistentId);
    if (objectIds_.count(persistentId))
        throw std::invalid_argument("Duplicate entity identity");
    auto batch = changes();
    Entity e = storage_.registry.create();
    storage_.registry.emplace<Identity>(e, Identity{std::move(name), persistentId});
    objectIds_.emplace(std::move(persistentId), e);
    batch.commit();
    return e;
}
ObjectPath World::objectPath(Entity e) const {
    return ObjectPath(resources.mapAsset.path, get<Identity>(e).persistentId);
}
Entity World::findObject(const std::string& id) const {
    auto p = objectIds_.find(id);
    return p == objectIds_.end() ? 0 : p->second;
}
Entity World::resolveObject(const ObjectPath& path) const {
    return path.map == resources.mapAsset.path ? findObject(path.object) : 0;
}
void World::setEnabled(Entity e, bool value) {
    storage_.registry.require(e);
    auto batch = changes();
    if (value)
        storage_.registry.remove<Disabled>(e);
    else if (!has<Disabled>(e))
        storage_.registry.emplace<Disabled>(e);
    transforms.refreshEnabled(e);
    batch.commit();
}
void World::destroy(Entity e) {
    storage_.registry.require(e);
    auto batch = changes();
    if (auto t = registry().tryGet<Transform>(e)) {
        auto children = t->children;
        for (auto child : children)
            destroy(child);
    }
    auto id = get<Identity>(e).persistentId;
    componentCatalog().destroy(*this, e);
    objectIds_.erase(id);
    for (auto it = resources.references.begin(); it != resources.references.end();)
        if (it->second == id)
            it = resources.references.erase(it);
        else
            ++it;
    storage_.registry.destroy(e);
    if (gameplay.selected == e)
        gameplay.selected = 0;
    if (gameplay.hovered == e)
        gameplay.hovered = 0;
    if (gameplay.playerId == e) {
        gameplay.playerId = 0;
        gameplay.path.clear();
        gameplay.hasDestination = false;
    }
    batch.commit();
}
void World::clearScene() {
    auto batch = changes();
    for (auto e : registry().entities())
        if (registry().contains(e))
            destroy(e);
    resources = {};
    gameplay = {};
    resetHistory = true;
    batch.commit();
}
static vec3 rayDirection(const Camera& camera, float x, float y, const Input& input) {
    auto inv = glm::inverse(camera.projection(float(std::max(input.width, 1u)) / std::max(input.height, 1u)) *
                            camera.view());
    auto v = inv * vec4(2 * x / std::max(input.width, 1u) - 1, 2 * y / std::max(input.height, 1u) - 1, 1, 1);
    return glm::normalize(vec3(v) / v.w - camera.eye());
}
std::optional<vec3> World::groundAt(float x, float y, const Input& input) const {
    QueryFilter filter;
    filter.walkableOnly = true;
    filter.mask = CollisionLayer::World;
    auto hit =
        physics().raycast(resources.camera.eye(), rayDirection(resources.camera, x, y, input), 160, filter);
    return hit ? std::optional<vec3>(hit->position) : std::nullopt;
}
Entity World::pick(float x, float y, const Input& input) const {
    QueryFilter filter;
    filter.pickableOnly = true;
    auto hit =
        physics().raycast(resources.camera.eye(), rayDirection(resources.camera, x, y, input), 160, filter);
    if (!hit || !registry().contains(hit->owner))
        return 0;
    return has<Interactable>(hit->owner) || hit->owner == gameplay.playerId ? hit->owner : 0;
}
Frame World::snapshot(const Input& input, uint64_t tick, double time, int debug, bool physicsDebug) {
    storage_.changes.requireCommitted();
    CpuScope scope("ECS Render Extraction");
    Frame f;
    render.extract(f, physicsDebug);
    f.proxies = storage_.renderScene.proxies();
    f.delta = storage_.renderScene.publish();
    f.materials = resources.materials;
    f.camera = resources.camera;
    f.input = input;
    if (auto t = registry().tryGet<Transform>(gameplay.playerId))
        f.player = t->world.position;
    f.selected = gameplay.selected;
    f.hovered = gameplay.hovered;
    f.destination = gameplay.destination;
    f.hasDestination = gameplay.hasDestination;
    f.tick = tick;
    f.time = time;
    f.resetHistory = resetHistory;
    f.debugView = debug;
    if (physicsDebug)
        appendPhysicsDebug(physics(), f);
    resetHistory = false;
    return f;
}
} // namespace afterlight
