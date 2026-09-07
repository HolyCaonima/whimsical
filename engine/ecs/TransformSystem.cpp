#include "Systems.h"
#include <algorithm>
namespace afterlight {
static PhysicsPose compose(PhysicsPose a, PhysicsPose b) {
    return {a.position + a.rotation * b.position, glm::normalize(a.rotation * b.rotation)};
}
static PhysicsPose relative(PhysicsPose parent, PhysicsPose world) {
    auto inverse = glm::inverse(parent.rotation);
    return {inverse * (world.position - parent.position), glm::normalize(inverse * world.rotation)};
}
static void validatePose(const PhysicsPose& p) {
    auto q = glm::length(p.rotation);
    if (!std::isfinite(p.position.x) || !std::isfinite(p.position.y) || !std::isfinite(p.position.z) ||
        !std::isfinite(q) || std::abs(q - 1) > .001f)
        throw std::invalid_argument("Expected finite position and unit rotation");
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
    validatePose(pose);
    s.registry.emplace<Transform>(e, Transform{pose, pose});
}
void TransformSystem::remove(Entity e) {
    s.registry.require(e);
    auto t = s.registry.tryGet<Transform>(e);
    if (!t)
        return;
    if (s.registry.has<Renderable>(e) || s.registry.has<Collider>(e) || s.registry.has<Animator>(e) ||
        s.registry.has<JointPose>(e))
        throw std::logic_error("Remove spatial capabilities before Transform");
    auto children = t->children;
    for (auto child : children)
        setParent(child, 0);
    setParent(e, 0);
    s.registry.remove<Transform>(e);
}
void TransformSystem::propagate(Entity e) {
    auto& t = s.registry.get<Transform>(e);
    t.world = t.parent ? compose(s.registry.get<Transform>(t.parent).world, t.local) : t.local;
    if (auto c = s.registry.tryGet<Collider>(e))
        s.physics.setPose(c->body, t.world);
    if (auto j = s.registry.tryGet<JointPose>(e))
        s.jointColliders.update(e, t.world, j->model);
    render.publishTransform(e);
    for (auto child : t.children)
        propagate(child);
}
void TransformSystem::setLocal(Entity e, const PhysicsPose& pose) {
    validatePose(pose);
    auto& t = s.registry.get<Transform>(e);
    if (t.local.position == pose.position && t.local.rotation == pose.rotation)
        return;
    t.local = pose;
    propagate(e);
}
void TransformSystem::setTransform(Entity e, const PhysicsPose& pose) {
    validatePose(pose);
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
    if (t.parent) {
        auto& siblings = s.registry.get<Transform>(t.parent).children;
        siblings.erase(std::find(siblings.begin(), siblings.end(), e));
    }
    t.parent = parent;
    if (parent)
        s.registry.get<Transform>(parent).children.push_back(e);
    if (keepWorld)
        t.local = parent ? relative(s.registry.get<Transform>(parent).world, t.world) : t.world;
    propagate(e);
    refreshEnabled(e);
}
void TransformSystem::refreshEnabled(Entity e) {
    bool enabled = s.enabled(e);
    if (auto c = s.registry.tryGet<Collider>(e))
        s.physics.setEnabled(c->body, enabled);
    s.jointColliders.setEnabled(e, enabled);
    render.publishAttributes(e);
    if (auto t = s.registry.tryGet<Transform>(e))
        for (auto child : t->children)
            refreshEnabled(child);
}
} // namespace afterlight
