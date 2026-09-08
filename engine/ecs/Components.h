#pragma once
#include "Registry.h"
#include "core/Types.h"
#include "physics/PhysicsScene.h"
#include "animation/SkinnedMesh.h"
#include "assets/RenderTargetAsset.h"
#include <optional>

namespace afterlight {
// Backend-owned components expose writes through their system/contract, never
// through an unchecked generic edit. New types opt in without a World blacklist.
struct SystemComponent {};
struct Identity {
    using Ownership = SystemComponent;
    std::string name, persistentId;
};
struct Disabled {
    using Ownership = SystemComponent;
};
struct Interactable {};
struct ScriptData {
    Json value = Json::object();
};
// Hierarchies are rigid (translation + rotation). Geometry dimensions belong to render
// and collider components; nonuniform parent scale cannot represent a rigid collider.
struct Transform {
    using Ownership = SystemComponent;
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
    using Ownership = SystemComponent;
    RenderComponent appearance;
    std::shared_ptr<const StaticMesh> mesh;
    uint32_t slot = UINT32_MAX; // Derived render-scene binding; never serialized.
};
struct DrawEntityID {
    using Ownership = SystemComponent;
    std::shared_ptr<const RenderTargetAsset> target;
};
// PhysicsScene owns shape/material/query state. This is the sole ECS binding to it;
// its pose is a derived spatial index of Transform, not another editable transform.
struct Collider {
    using Ownership = SystemComponent;
    BodyHandle body;
};
struct JointPose {
    using Ownership = SystemComponent;
    std::vector<PhysicsPose> model;
    std::shared_ptr<const animation::Skeleton> skeleton; // Optional layout, independent of a solver.
};
struct JointColliders {
    using Ownership = SystemComponent;
};
struct RootMotionBinding {
    using Ownership = SystemComponent;
    enum class Mode { Transform, Kinematic, Grounded };
    Mode mode = Mode::Transform;
    uint32_t mask = CollisionLayer::All;
    bool preserveAnchor = false;
};
struct Animator {
    using Ownership = SystemComponent;

  private:
    std::unique_ptr<animation::Instance> instance;
    friend class AnimationSystem;
    friend class RenderSystem;
    friend class ScenePersistence;

  public:
    Animator(std::unique_ptr<animation::Instance> value, vec3 offset)
        : instance(std::move(value)), rootOffset(offset) {}
    const animation::Instance& solver() const {
        return *instance;
    }
    animation::Input input;
    std::optional<AssetRef> asset;
    vec3 rootOffset{0};
};
struct Skin {
    using Ownership = SystemComponent;
    std::shared_ptr<const SkinnedMesh> mesh;
    std::vector<unsigned> joints;
};
} // namespace afterlight
