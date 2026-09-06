#include "AnimationCollision.h"
#include <algorithm>
#include <stdexcept>
namespace afterlight {
std::vector<AnimationColliderDescription> AnimationCollision::describe(uint32_t owner) const {
    std::vector<AnimationColliderDescription> result;
    for (const auto& b : bindings_)
        if (b.owner == owner) {
            const auto& body = scene_.body(b.body);
            result.push_back({b.joint, body.shape, b.local, body.blocking});
        }
    return result;
}
static PhysicsPose combine(PhysicsPose a, PhysicsPose b) {
    return {a.position + a.rotation * b.position, glm::normalize(a.rotation * b.rotation)};
}
static void validPose(const PhysicsPose& p) {
    if (!std::isfinite(p.position.x) || !std::isfinite(p.position.y) || !std::isfinite(p.position.z) ||
        !std::isfinite(glm::dot(p.rotation, p.rotation)) ||
        std::abs(glm::dot(p.rotation, p.rotation) - 1) > .001f)
        throw std::invalid_argument("Animation physics pose must be finite and rigid");
}
BodyHandle AnimationCollision::bind(uint32_t owner, uint32_t joint, const ColliderShape& shape,
                                    PhysicsPose local, bool blocking) {
    PhysicsBody body;
    body.owner = owner;
    body.shape = shape;
    body.pose = local;
    body.motion = BodyMotion::Kinematic;
    body.layer = blocking ? CollisionLayer::Character : CollisionLayer::Trigger;
    body.blocking = blocking;
    body.pickable = false;
    body.enabled = false;
    auto handle = scene_.create(body);
    bindings_.push_back({owner, joint, handle, local});
    return handle;
}
void AnimationCollision::update(uint32_t owner, PhysicsPose root, const std::vector<PhysicsPose>& joints) {
    validPose(root);
    for (const auto& pose : joints)
        validPose(pose);
    for (const auto& b : bindings_)
        if (b.owner == owner && b.joint >= joints.size())
            throw std::out_of_range("Animation collider references a missing joint");
    for (const auto& b : bindings_)
        if (b.owner == owner)
            scene_.setPose(b.body, combine(combine(root, joints[b.joint]), b.local));
}
void AnimationCollision::setEnabled(uint32_t owner, bool enabled) {
    for (const auto& b : bindings_)
        if (b.owner == owner)
            scene_.setEnabled(b.body, enabled);
}
void AnimationCollision::remove(uint32_t owner) {
    for (const auto& b : bindings_)
        if (b.owner == owner)
            scene_.destroy(b.body);
    bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(),
                                   [&](const Binding& b) { return b.owner == owner; }),
                    bindings_.end());
}
} // namespace afterlight
