#include "Systems.h"
#include "physics/SpatialCollider.h"
namespace whimsical {
MotionSystem::MotionSystem(SceneStorage& storage, TransformSystem& t, NavigationSettings& n)
    : s(storage), transforms(t), navigation(n) {
    s.changes.subscribe<WorldPoseChanged>([this](Entity e) {
        if (auto c = s.registry.tryGet<Collider>(e)) {
            auto spatial = worldCollider(c->shape, s.registry.get<Transform>(e).world);
            s.physics.setShape(c->body, spatial.shape);
            s.physics.setPose(c->body, spatial.pose);
        }
    });
    s.changes.subscribe<EffectiveEnabledChanged>([this](Entity e) {
        if (auto c = s.registry.tryGet<Collider>(e))
            s.physics.setEnabled(c->body, s.enabled(e));
    });
}

void MotionSystem::add(Entity e, PhysicsBody body) {
    auto batch = Changes::Batch(s.changes);
    auto& t = s.registry.get<Transform>(e);
    if (s.registry.has<Collider>(e))
        throw std::logic_error("Collider already present");
    body.owner = e;
    auto localShape = body.shape;
    auto spatial = worldCollider(localShape, t.world);
    body.shape = spatial.shape;
    body.pose = spatial.pose;
    body.enabled = s.enabled(e);
    auto h = s.physics.create(body);
    s.registry.emplace<Collider>(e, Collider{h, localShape});
    batch.commit();
}
ColliderSettings MotionSystem::collider(Entity e) const {
    const auto& b = s.physics.body(s.registry.get<Collider>(e).body);
    return {s.registry.get<Collider>(e).shape, b.motion, b.layer, b.blocking, b.walkable, b.pickable};
}
void MotionSystem::set(Entity e, PhysicsBody body) {
    // Shape validation happens before any property changes; the binding stays stable.
    auto batch = Changes::Batch(s.changes);
    setColliderShape(e, body.shape);
    auto h = s.registry.get<Collider>(e).body;
    s.physics.setMotion(h, body.motion);
    configureCollider(e, body.layer, body.blocking, body.walkable, body.pickable);
    batch.commit();
}
void MotionSystem::remove(Entity e) {
    s.removeComponent(e, typeid(Collider));
}
void MotionSystem::configureCollider(Entity e, uint32_t layer, bool blocking, bool walkable, bool pickable) {
    auto batch = Changes::Batch(s.changes);
    s.physics.setProperties(s.registry.get<Collider>(e).body, layer, blocking, walkable, pickable);
    s.changes.mark<Collider>(e);
    batch.commit();
}
void MotionSystem::setColliderShape(Entity e, const ColliderShape& shape) {
    auto batch = Changes::Batch(s.changes);
    if (auto a = s.registry.tryGet<RootMotionBinding>(e);
        a && a->mode == RootMotionBinding::Mode::Grounded && shape.type != ColliderType::Capsule)
        throw std::logic_error("Root motion requires a capsule collider");
    auto& collider = s.registry.get<Collider>(e);
    auto spatial = worldCollider(shape, s.registry.get<Transform>(e).world);
    s.physics.setShape(collider.body, spatial.shape);
    collider.shape = shape;
    s.changes.mark<Collider>(e);
    batch.commit();
}
void MotionSystem::setSolid(Entity e, bool solid) {
    auto h = s.registry.get<Collider>(e).body;
    auto b = s.physics.body(h);
    configureCollider(e, b.layer, solid, b.walkable, b.pickable);
}
NavigationAgent MotionSystem::agent(Entity e) const {
    s.changes.requireCommitted();
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
vec3 MotionSystem::characterPosition(Entity e, vec3 delta) const {
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
    return accepted;
}
vec3 MotionSystem::moveCharacter(Entity e, vec3 delta) {
    auto pose = s.registry.get<Transform>(e).world;
    pose.position = characterPosition(e, delta);
    transforms.setWorld(e, pose);
    return pose.position;
}
vec3 MotionSystem::rootMotion(Entity e, vec3 delta, float yaw) {
    if (!std::isfinite(yaw))
        throw std::invalid_argument("Invalid root rotation");
    auto rotation = s.registry.get<Transform>(e).world.rotation;
    auto p = characterPosition(e, rotation * (s.registry.get<Transform>(e).world.scale * delta));
    transforms.setTransform(e, {p, glm::normalize(rotation * glm::angleAxis(yaw, vec3(0, 1, 0)))});
    return p;
}
BodyMoveResult MotionSystem::moveBody(Entity e, vec3 delta, quat rotation, uint32_t mask) {
    s.changes.requireCommitted();
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
    s.changes.requireCommitted();
    auto h = s.registry.get<Collider>(e).body;
    auto pose = s.registry.get<Transform>(e).world;
    auto before = pose.position;
    auto batch = Changes::Batch(s.changes);
    s.physics.resizeCharacter(h, height);
    pose.position = s.physics.body(h).pose.position;
    auto shape = s.physics.body(h).shape;
    s.registry.get<Collider>(e).shape = ColliderShape::capsule(shape.radius / pose.scale.x, shape.height() / pose.scale.x);
    s.changes.emit(CharacterResized{e, pose.position - before});
    transforms.setWorld(e, pose);
    s.changes.mark<Collider>(e);
    batch.commit();
    return s.physics.body(h).shape.height();
}
void MotionSystem::bindRootMotion(Entity e, RootMotionBinding binding) {
    auto batch = Changes::Batch(s.changes);
    s.registry.get<Animator>(e);
    if (binding.mode == RootMotionBinding::Mode::Grounded)
        if (collider(e).shape.type != ColliderType::Capsule)
            throw std::invalid_argument("Grounded root motion requires capsule");
    if (binding.mode == RootMotionBinding::Mode::Kinematic)
        s.registry.get<Collider>(e);
    if (auto current = s.registry.tryGet<RootMotionBinding>(e)) {
        *current = binding;
        s.changes.mark<RootMotionBinding>(e);
    } else
        s.registry.emplace<RootMotionBinding>(e, binding);
    batch.commit();
}
PhysicsPose MotionSystem::solveRootMotion(Entity e, const animation::Transform& delta, vec3 offset) const {
    s.changes.requireCommitted();
    auto pose = s.registry.get<Transform>(e).world;
    auto binding = s.registry.tryGet<RootMotionBinding>(e);
    if (!binding)
        return {pose.position, pose.rotation};
    auto translation = pose.rotation * (pose.scale * (delta.position + offset) - delta.rotation * (pose.scale * offset));
    auto rotation = glm::normalize(pose.rotation * delta.rotation);
    switch (binding->mode) {
    case RootMotionBinding::Mode::Transform:
        return {pose.position + translation, rotation};
    case RootMotionBinding::Mode::Kinematic: {
        const auto& body = s.physics.body(s.registry.get<Collider>(e).body);
        QueryFilter filter;
        filter.mask = binding->mask;
        filter.ignoreOwner = e;
        filter.blockingOnly = true;
        return s.physics.moveAndSlide({body.shape, body.pose}, translation, rotation, filter).pose;
    }
    case RootMotionBinding::Mode::Grounded: {
        auto forward = delta.rotation * vec3(0, 0, 1);
        return {
            characterPosition(e, translation),
            glm::normalize(pose.rotation * glm::angleAxis(std::atan2(forward.x, forward.z), vec3(0, 1, 0)))};
    }
    }
    throw std::invalid_argument("Unknown root motion mode");
}
std::vector<vec3> MotionSystem::findPath(Entity e, vec3 target) const {
    return Navigation::findPath(s.physics, feet(e), target, agent(e), navigation);
}
} // namespace whimsical
