#pragma once
#include "physics/PhysicsScene.h"
#include "core/SpatialTransform.h"
namespace whimsical {
struct AnimationColliderDescription {
    uint32_t joint = 0;
    ColliderShape shape;
    PhysicsPose local;
    bool blocking = false;
};
// Animation publishes model-space rigid transforms relative to the actor root (not parent-local).
// PhysicsScene owns the resulting colliders.
class AnimationCollision {
    struct Binding {
        uint32_t owner, joint;
        BodyHandle body;
        PhysicsPose local;
        ColliderShape shape;
    };
    PhysicsScene& scene_;
    std::vector<Binding> bindings_;

  public:
    explicit AnimationCollision(PhysicsScene& scene) : scene_(scene) {}
    void exchangeBindings(AnimationCollision& other) {
        bindings_.swap(other.bindings_);
    }
    BodyHandle bind(uint32_t owner, uint32_t joint, const ColliderShape&, PhysicsPose local,
                    bool blocking = false);
    void replace(uint32_t owner, const std::vector<AnimationColliderDescription>&, const TransformPose& root,
                 const std::vector<PhysicsPose>& joints, bool enabled);
    void update(uint32_t owner, const TransformPose& root, const std::vector<PhysicsPose>& joints);
    void setEnabled(uint32_t owner, bool);
    void remove(uint32_t owner);
    std::vector<AnimationColliderDescription> describe(uint32_t owner) const;
};
} // namespace whimsical
