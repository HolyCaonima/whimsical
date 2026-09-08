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
Entity World::create(std::string name, std::string persistentId, bool persistent) {
    if (persistentId.empty())
        persistentId = newPersistentId();
    validatePersistentId(persistentId);
    if (objectIds_.count(persistentId))
        throw std::invalid_argument("Duplicate entity identity");
    auto batch = changes();
    Entity e = storage_.registry.create();
    storage_.registry.emplace<Identity>(e, Identity{std::move(name), persistentId, persistent});
    objectIds_.emplace(std::move(persistentId), e);
    batch.commit();
    return e;
}
void World::rename(Entity e, std::string name) {
    auto batch = changes();
    storage_.registry.get<Identity>(e).name = std::move(name);
    storage_.changes.mark<Identity>(e);
    batch.commit();
}
void World::setPersistent(Entity e, bool value) {
    auto batch = changes();
    storage_.registry.get<Identity>(e).persistent = value;
    storage_.changes.mark<Identity>(e);
    batch.commit();
}
bool World::persistent(Entity e) const {
    do {
        if (!get<Identity>(e).persistent)
            return false;
        auto t = registry().tryGet<Transform>(e);
        e = t ? t->parent : 0;
    } while (e);
    return true;
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
    batch.commit();
}
void World::clearScene() {
    renderTargets.reset();
    auto batch = changes();
    for (auto e : registry().entities())
        if (registry().contains(e))
            destroy(e);
    resources = {};
    resetHistory = true;
    batch.commit();
}
static vec3 rayDirection(const Camera& camera, float x, float y, const Input& input) {
    auto inv = glm::inverse(camera.projection(float(std::max(input.width, 1u)) / std::max(input.height, 1u)) *
                            camera.view());
    auto v = inv * vec4(2 * x / std::max(input.width, 1u) - 1, 2 * y / std::max(input.height, 1u) - 1, 1, 1);
    return glm::normalize(vec3(v) / v.w - camera.eye());
}
std::optional<vec3> World::groundAt(float x, float y, const Input& input,
                                    const Camera* cameraOverride) const {
    const auto& camera = cameraOverride ? *cameraOverride : resources.camera;
    QueryFilter filter;
    filter.walkableOnly = true;
    filter.mask = CollisionLayer::World;
    auto hit = physics().raycast(camera.eye(), rayDirection(camera, x, y, input), 160, filter);
    return hit ? std::optional<vec3>(hit->position) : std::nullopt;
}
Entity World::pick(float x, float y, const Input& input, const Camera* cameraOverride) const {
    const auto& camera = cameraOverride ? *cameraOverride : resources.camera;
    QueryFilter filter;
    filter.pickableOnly = true;
    auto hit = physics().raycast(camera.eye(), rayDirection(camera, x, y, input), 160, filter);
    if (!hit || !registry().contains(hit->owner))
        return 0;
    return hit->owner;
}
Frame World::snapshot(const Input& input, uint64_t tick, double time, int debug, bool physicsDebug,
                      const RenderView* view) {
    storage_.changes.requireCommitted();
    CpuScope scope("ECS Render Extraction");
    Frame f;
    render.extract(f, physicsDebug, renderTargets);
    f.pixelReads = renderTargets.snapshot(tick);
    f.proxies = storage_.renderScene.proxies();
    f.delta = storage_.renderScene.publish();
    f.materials = resources.materials;
    f.camera = view && view->camera ? *view->camera : resources.camera;
    f.viewport = (view ? view->rectangle : ViewRect{}).fit(input.width, input.height);
    f.input = input;
    // Presentation requests never become authored components or gameplay state.
    // Sorting and coalescing here gives GPU lookup logarithmic cost; later layers win.
    std::map<uint32_t, vec4> outlines;
    if (view)
        for (const auto& outline : view->outlines)
            if (auto r = registry().tryGet<Renderable>(outline.entity);
                r && enabled(outline.entity) && r->appearance.visible && !r->appearance.overlay)
                outlines[outline.entity] = outline.color;
    for (const auto& [entity, color] : outlines)
        f.outlines.push_back({entity, color});
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
