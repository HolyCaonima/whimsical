#include "Systems.h"
#include "Validation.h"
#include "physics/SpatialCollider.h"
#include <algorithm>
namespace whimsical {
bool SceneStorage::enabled(Entity e) const {
    while (e) {
        if (registry.has<Disabled>(e))
            return false;
        auto t = registry.tryGet<Transform>(e);
        e = t ? t->parent : 0;
    }
    return true;
}
void TransformSystem::add(Entity e, TransformPose pose) {
    auto batch = Changes::Batch(s.changes);
    validateTransformPose(pose);
    s.registry.emplace<Transform>(e, Transform{pose, pose});
    batch.commit();
}
void TransformSystem::remove(Entity e) {
    s.removeComponent(e, typeid(Transform));
}
void TransformSystem::propagate(Entity e) {
    auto& t = s.registry.get<Transform>(e);
    t.world = t.parent ? composeTransform(s.registry.get<Transform>(t.parent).world, t.local) : t.local;
    s.changes.mark<WorldPoseChanged>(e);
    for (auto child : t.children)
        propagate(child);
}
void TransformSystem::setLocal(Entity e, const TransformPose& pose) {
    setLocal(e, pose, s.registry.get<Transform>(e).parent);
}
void TransformSystem::setLocal(Entity e, const TransformPose& pose, Entity parent) {
    setLocalAs(e, pose, parent, typeid(void));
}
void TransformSystem::setLocalAs(Entity e, const TransformPose& pose, Entity parent, std::type_index writer) {
    validateTransformPose(pose);
    auto& t = s.registry.get<Transform>(e);
    if (t.driver != writer)
        throw std::logic_error("Local transform is owned by another driver");
    if (parent) {
        s.registry.get<Transform>(parent);
        for (auto p = parent; p; p = s.registry.get<Transform>(p).parent)
            if (p == e)
                throw std::invalid_argument("Transform hierarchy cycle");
    }
    bool reparent = t.parent != parent;
    if (!reparent && t.local.position == pose.position && t.local.rotation == pose.rotation &&
        t.local.scale == pose.scale)
        return;
    validateSubtree(e, parent ? composeTransform(s.registry.get<Transform>(parent).world, pose) : pose);
    auto batch = Changes::Batch(s.changes);
    if (reparent) {
        if (t.parent) {
            auto& siblings = s.registry.get<Transform>(t.parent).children;
            siblings.erase(std::find(siblings.begin(), siblings.end(), e));
        }
        t.parent = parent;
        if (parent)
            s.registry.get<Transform>(parent).children.push_back(e);
    }
    t.local = pose;
    s.changes.mark<Transform>(e);
    propagate(e);
    if (reparent)
        refreshEnabled(e);
    batch.commit();
}
void TransformSystem::claim(Entity e, std::type_index owner) {
    auto& t = s.registry.get<Transform>(e);
    if (owner == typeid(void) || (t.driver != typeid(void) && t.driver != owner))
        throw std::logic_error("Transform already has a driver");
    t.driver = owner;
}
void TransformSystem::release(Entity e, std::type_index owner) {
    auto& t = s.registry.get<Transform>(e);
    if (t.driver == owner)
        t.driver = typeid(void);
}
void TransformSystem::setDrivenLocal(Entity e, const TransformPose& pose, std::type_index owner) {
    setLocalAs(e, pose, s.registry.get<Transform>(e).parent, owner);
}
void TransformSystem::validateSubtree(Entity e, const TransformPose& world) const {
    validateTransformPose(world);
    if (auto c = s.registry.tryGet<Collider>(e))
        (void)worldCollider(c->shape, world);
    if (s.registry.has<JointColliders>(e))
        for (const auto& c : s.jointColliders.describe(e))
            (void)scaledCollider(c.shape, world.scale);
    for (auto child : s.registry.get<Transform>(e).children)
        validateSubtree(child, composeTransform(world, s.registry.get<Transform>(child).local));
}
void TransformSystem::setWorld(Entity e, const TransformPose& pose) {
    validateTransformPose(pose);
    auto& t = s.registry.get<Transform>(e);
    setLocal(e, t.parent ? relativeTransform(s.registry.get<Transform>(t.parent).world, pose) : pose);
}
void TransformSystem::setTransform(Entity e, const PhysicsPose& pose) {
    auto world = s.registry.get<Transform>(e).world;
    world.position = pose.position;
    world.rotation = pose.rotation;
    setWorld(e, world);
}
void TransformSystem::setScale(Entity e, vec3 scale) {
    auto local = s.registry.get<Transform>(e).local;
    local.scale = scale;
    setLocal(e, local);
}
void TransformSystem::setParent(Entity e, Entity parent, bool keepWorld) {
    const auto& t = s.registry.get<Transform>(e);
    auto local = keepWorld ? (parent ? relativeTransform(s.registry.get<Transform>(parent).world, t.world)
                                    : t.world)
                           : t.local;
    setLocal(e, local, parent);
}
void TransformSystem::refreshEnabled(Entity e) {
    auto batch = Changes::Batch(s.changes);
    s.changes.mark<EffectiveEnabledChanged>(e);
    if (auto t = s.registry.tryGet<Transform>(e))
        for (auto child : t->children)
            refreshEnabled(child);
    batch.commit();
}
} // namespace whimsical
