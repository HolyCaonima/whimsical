#pragma once
#include "physics/PhysicsScene.h"
namespace afterlight {
// Animation publishes joint-local rigid transforms; PhysicsScene owns the resulting colliders.
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
    BodyHandle bind(uint32_t owner, uint32_t joint, const ColliderShape&, PhysicsPose local,
                    bool blocking = false);
    void update(uint32_t owner, PhysicsPose root, const std::vector<PhysicsPose>& joints);
    void setEnabled(uint32_t owner, bool);
    void remove(uint32_t owner);
};
} // namespace afterlight
