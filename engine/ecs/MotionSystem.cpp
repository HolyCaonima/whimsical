#include "Systems.h"
namespace afterlight {
MotionSystem::MotionSystem(SceneStorage& storage, TransformSystem& t, NavigationSettings& n)
    : s(storage), transforms(t), navigation(n) {
    s.changes.subscribe<WorldPoseChanged>([this](Entity e) {
        if (auto c = s.registry.tryGet<Collider>(e))
            s.physics.setPose(c->body, s.registry.get<Transform>(e).world);
    });
    s.changes.subscribe<EffectiveEnabledChanged>([this](Entity e) {
        if (auto c = s.registry.tryGet<Collider>(e))
            s.physics.setEnabled(c->body, s.enabled(e));
    });
}

void MotionSystem::add(Entity e, PhysicsBody body) {
    auto& t = s.registry.get<Transform>(e);
    if (s.registry.has<Collider>(e))
        throw std::logic_error("Collider already present");
    body.owner = e;
    body.pose = t.world;
    body.enabled = s.enabled(e);
    auto h = s.physics.create(body);
    s.registry.emplace<Collider>(e, Collider{h});
}
void MotionSystem::remove(Entity e) {
    s.removeComponent(e, typeid(Collider));
}
void MotionSystem::configureCollider(Entity e, uint32_t layer, bool blocking, bool walkable, bool pickable) {
    s.physics.setProperties(s.registry.get<Collider>(e).body, layer, blocking, walkable, pickable);
}
void MotionSystem::setColliderShape(Entity e, const ColliderShape& shape) {
    if (auto a = s.registry.tryGet<RootMotionBinding>(e);
        a && a->mode == RootMotionBinding::Mode::Grounded && shape.type != ColliderType::Capsule)
        throw std::logic_error("Root motion requires a capsule collider");
    s.physics.setShape(s.registry.get<Collider>(e).body, shape);
}
void MotionSystem::setSolid(Entity e, bool solid) {
    auto h = s.registry.get<Collider>(e).body;
    auto b = s.physics.body(h);
    s.physics.setProperties(h, b.layer, solid, b.walkable, b.pickable);
}
NavigationAgent MotionSystem::agent(Entity e) const {
    const auto& b = s.physics.body(s.registry.get<Collider>(e).body);
    if (b.shape.type != ColliderType::Capsule)
        throw std::invalid_argument("Movement requires capsule collider");
    if (glm::distance(b.pose.rotation * vec3(0, 1, 0), vec3(0, 1, 0)) > .001f)
        throw std::invalid_argument("Grounded movement requires upright capsule; use moveBody");
    return {b.shape.radius, b.shape.height(), e};
}
vec3 MotionSystem::feet(Entity e) const {
    return s.registry.get<Transform>(e).world.position - vec3(0, agent(e).height * .5f, 0);
}
vec3 MotionSystem::moveCharacter(Entity e, vec3 delta) {
    auto a = agent(e);
    auto pose = s.registry.get<Transform>(e).world;
    QueryFilter filter;
    filter.mask = CollisionLayer::World | CollisionLayer::Character;
    filter.ignoreOwner = e;
    filter.blockingOnly = true;
    auto move = s.physics.moveAndSlide({pose.position, a.radius, a.height}, delta, filter);
    vec3 accepted = pose.position;
    int steps = std::max(1, int(std::ceil(glm::length(move.applied) / .08f)));
    for (int i = 1; i <= steps; ++i) {
        vec3 p = glm::mix(pose.position, move.position, float(i) / steps);
        if (!Navigation::canStand(s.physics, p - vec3(0, a.height * .5f, 0), a, navigation))
            break;
        accepted = p;
    }
    pose.position = accepted;
    transforms.setTransform(e, pose);
    return accepted;
}
vec3 MotionSystem::rootMotion(Entity e, vec3 delta, float yaw) {
    if (!std::isfinite(yaw))
        throw std::invalid_argument("Invalid root rotation");
    auto rotation = s.registry.get<Transform>(e).world.rotation;
    auto p = moveCharacter(e, rotation * delta);
    transforms.setTransform(e, {p, glm::normalize(rotation * glm::angleAxis(yaw, vec3(0, 1, 0)))});
    return p;
}
BodyMoveResult MotionSystem::moveBody(Entity e, vec3 delta, quat rotation, uint32_t mask) {
    auto body = s.physics.body(s.registry.get<Collider>(e).body);
    QueryFilter filter;
    filter.mask = mask;
    filter.ignoreOwner = e;
    filter.blockingOnly = true;
    auto result = s.physics.moveAndSlide({body.shape, body.pose}, delta, rotation, filter);
    transforms.setTransform(e, result.pose);
    return result;
}
float MotionSystem::setCharacterHeight(Entity e, float height) {
    auto h = s.registry.get<Collider>(e).body;
    auto pose = s.registry.get<Transform>(e).world;
    float before = pose.position.y;
    s.physics.resizeCharacter(h, height);
    pose.position = s.physics.body(h).pose.position;
    s.changes.emit(CharacterResized{e, pose.position.y - before});
    transforms.setTransform(e, pose);
    return s.physics.body(h).shape.height();
}
void MotionSystem::bindRootMotion(Entity e, RootMotionBinding binding) {
    s.registry.get<Animator>(e);
    if (binding.mode == RootMotionBinding::Mode::Grounded)
        (void)agent(e);
    if (binding.mode == RootMotionBinding::Mode::Kinematic)
        s.registry.get<Collider>(e);
    s.registry.emplace<RootMotionBinding>(e, binding);
}
void MotionSystem::consumeRootMotion(Entity e, const animation::Transform& delta, vec3 offset) {
    auto binding = s.registry.tryGet<RootMotionBinding>(e);
    if (!binding)
        return;
    auto pose = s.registry.get<Transform>(e).world;
    auto translation = delta.position + offset - delta.rotation * offset;
    auto rotation = glm::normalize(pose.rotation * delta.rotation);
    switch (binding->mode) {
    case RootMotionBinding::Mode::Transform:
        transforms.setTransform(e, {pose.position + pose.rotation * translation, rotation});
        break;
    case RootMotionBinding::Mode::Kinematic:
        moveBody(e, pose.rotation * translation, rotation, binding->mask);
        break;
    case RootMotionBinding::Mode::Grounded: {
        auto forward = delta.rotation * vec3(0, 0, 1);
        rootMotion(e, translation, std::atan2(forward.x, forward.z));
        break;
    }
    }
}
std::vector<vec3> MotionSystem::findPath(Entity e, vec3 target) const {
    return Navigation::findPath(s.physics, feet(e), target, agent(e), navigation);
}
} // namespace afterlight
