#pragma once
#include "physics/PhysicsScene.h"
namespace afterlight {
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
    void update(uint32_t owner, PhysicsPose root, const std::vector<PhysicsPose>& joints);
    void setEnabled(uint32_t owner, bool);
    void remove(uint32_t owner);
    std::vector<AnimationColliderDescription> describe(uint32_t owner) const;
};
} // namespace afterlight
