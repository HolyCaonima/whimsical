#include "Systems.h"
#include "Validation.h"
#include <algorithm>
namespace afterlight {
static PhysicsPose compose(PhysicsPose a, PhysicsPose b) {
    return {a.position + a.rotation * b.position, glm::normalize(a.rotation * b.rotation)};
}
static PhysicsPose relative(PhysicsPose parent, PhysicsPose world) {
    auto inverse = glm::inverse(parent.rotation);
    return {inverse * (world.position - parent.position), glm::normalize(inverse * world.rotation)};
}
bool SceneStorage::enabled(Entity e) const {
    while (e) {
        if (registry.has<Disabled>(e))
            return false;
        auto t = registry.tryGet<Transform>(e);
        e = t ? t->parent : 0;
    }
    return true;
}
void TransformSystem::add(Entity e, PhysicsPose pose) {
    auto batch = Changes::Batch(s.changes);
    validateRigidPose(pose);
    s.registry.emplace<Transform>(e, Transform{pose, pose});
    batch.commit();
}
void TransformSystem::remove(Entity e) {
    s.removeComponent(e, typeid(Transform));
}
void TransformSystem::propagate(Entity e) {
    auto& t = s.registry.get<Transform>(e);
    t.world = t.parent ? compose(s.registry.get<Transform>(t.parent).world, t.local) : t.local;
    s.changes.mark<WorldPoseChanged>(e);
    for (auto child : t.children)
        propagate(child);
}
void TransformSystem::setLocal(Entity e, const PhysicsPose& pose) {
    validateRigidPose(pose);
    auto& t = s.registry.get<Transform>(e);
    if (t.local.position == pose.position && t.local.rotation == pose.rotation)
        return;
    auto batch = Changes::Batch(s.changes);
    t.local = pose;
    s.changes.mark<Transform>(e);
    propagate(e);
    batch.commit();
}
void TransformSystem::setTransform(Entity e, const PhysicsPose& pose) {
    validateRigidPose(pose);
    auto& t = s.registry.get<Transform>(e);
    setLocal(e, t.parent ? relative(s.registry.get<Transform>(t.parent).world, pose) : pose);
}
void TransformSystem::setParent(Entity e, Entity parent, bool keepWorld) {
    auto& t = s.registry.get<Transform>(e);
    if (parent) {
        s.registry.get<Transform>(parent);
        for (auto p = parent; p; p = s.registry.get<Transform>(p).parent)
            if (p == e)
                throw std::invalid_argument("Transform hierarchy cycle");
    }
    if (t.parent == parent)
        return;
    auto batch = Changes::Batch(s.changes);
    if (t.parent) {
        auto& siblings = s.registry.get<Transform>(t.parent).children;
        siblings.erase(std::find(siblings.begin(), siblings.end(), e));
    }
    t.parent = parent;
    s.changes.mark<Transform>(e);
    if (parent)
        s.registry.get<Transform>(parent).children.push_back(e);
    if (keepWorld)
        t.local = parent ? relative(s.registry.get<Transform>(parent).world, t.world) : t.world;
    propagate(e);
    refreshEnabled(e);
    batch.commit();
}
void TransformSystem::refreshEnabled(Entity e) {
    auto batch = Changes::Batch(s.changes);
    s.changes.mark<EffectiveEnabledChanged>(e);
    if (auto t = s.registry.tryGet<Transform>(e))
        for (auto child : t->children)
            refreshEnabled(child);
    batch.commit();
}
} // namespace afterlight
