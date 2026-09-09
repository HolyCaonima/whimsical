#include "AnimationCollision.h"
#include "physics/SpatialCollider.h"
#include <algorithm>
#include <stdexcept>
namespace whimsical {
std::vector<AnimationColliderDescription> AnimationCollision::describe(uint32_t owner) const {
    std::vector<AnimationColliderDescription> result;
    for (const auto& b : bindings_)
        if (b.owner == owner) {
            const auto& body = scene_.body(b.body);
            result.push_back({b.joint, b.shape, b.local, body.blocking});
        }
    return result;
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
    bindings_.push_back({owner, joint, handle, local, shape});
    return handle;
}
void AnimationCollision::replace(uint32_t owner,
                                 const std::vector<AnimationColliderDescription>& descriptions,
                                 const TransformPose& root, const std::vector<PhysicsPose>& joints, bool enabled) {
    AnimationCollision staged(scene_);
    try {
        for (const auto& b : descriptions) {
            if (b.joint >= joints.size())
                throw std::invalid_argument("Joint collider index outside pose");
            staged.bind(owner, b.joint, b.shape, b.local, b.blocking);
        }
        staged.update(owner, root, joints);
        staged.setEnabled(owner, enabled);
    } catch (...) {
        staged.remove(owner);
        throw;
    }
    remove(owner);
    bindings_.insert(bindings_.end(), staged.bindings_.begin(), staged.bindings_.end());
}
void AnimationCollision::update(uint32_t owner, const TransformPose& root, const std::vector<PhysicsPose>& joints) {
    validateTransformPose(root);
    for (const auto& pose : joints)
        validPose(pose);
    for (const auto& b : bindings_)
        if (b.owner == owner && b.joint >= joints.size())
            throw std::out_of_range("Animation collider references a missing joint");
    for (const auto& b : bindings_)
        if (b.owner == owner) {
            const auto& joint = joints[b.joint];
            auto world = composeTransform(composeTransform(root, {joint.position, joint.rotation}),
                                          {b.local.position, b.local.rotation});
            auto spatial = worldCollider(b.shape, world);
            scene_.setShape(b.body, spatial.shape);
            scene_.setPose(b.body, spatial.pose);
        }
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
} // namespace whimsical
