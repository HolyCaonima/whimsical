#pragma once
#include "Registry.h"
#include "core/Types.h"
#include "physics/PhysicsScene.h"
#include "animation/SkinnedMesh.h"
#include <optional>

namespace afterlight {
struct Identity {
    std::string name, persistentId;
};
struct Disabled {};
struct Interactable {};
struct ScriptData {
    Json value = Json::object();
};
// Hierarchies are rigid (translation + rotation). Geometry dimensions belong to render
// and collider components; nonuniform parent scale cannot represent a rigid collider.
struct Transform {
    PhysicsPose local;
    PhysicsPose world; // Derived cache, written only by TransformSystem.
    Entity parent = 0;
    std::vector<Entity> children;
    float yaw() const {
        auto forward = world.rotation * vec3(0, 0, 1);
        return std::atan2(forward.x, forward.z);
    }
};
struct Renderable {
    RenderComponent appearance;
    std::shared_ptr<const StaticMesh> mesh;
    uint32_t slot = UINT32_MAX; // Derived render-scene binding; never serialized.
};
// PhysicsScene owns shape/material/query state. This is the sole ECS binding to it;
// its pose is a derived spatial index of Transform, not another editable transform.
struct Collider {
    BodyHandle body;
};
struct JointPose {
    std::vector<PhysicsPose> model;
};
struct JointColliders {}; // Binding descriptions and bodies live in AnimationCollision.
struct Animator {
  private:
    std::unique_ptr<animation::Instance> instance;
    friend class AnimationSystem;
    friend class RenderSystem;
    friend class ScenePersistence;

  public:
    Animator(std::unique_ptr<animation::Instance> value, bool root, vec3 offset)
        : instance(std::move(value)), applyRootMotion(root), rootOffset(offset) {}
    const animation::Instance& solver() const {
        return *instance;
    }
    animation::Input input;
    std::optional<AssetRef> asset;
    bool applyRootMotion = true;
    vec3 rootOffset{0};
};
struct Skin {
    std::shared_ptr<const SkinnedMesh> mesh;
    std::vector<unsigned> joints;
};
} // namespace afterlight
