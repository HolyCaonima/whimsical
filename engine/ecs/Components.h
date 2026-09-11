#pragma once
#include "Registry.h"
#include "core/SpatialTransform.h"
#include "core/Types.h"
#include "physics/PhysicsScene.h"
#include "animation/SkinnedMesh.h"
#include "assets/RenderTargetAsset.h"
#include <optional>

namespace whimsical {
// Backend-owned components expose writes through their system/contract, never
// through an unchecked generic edit. New types opt in without a World blacklist.
struct SystemComponent {};
struct Identity {
    using Ownership = SystemComponent;
    std::string name, persistentId;
    bool persistent = true; // Local authoring membership; children inherit exclusion.
};
struct Disabled {
    using Ownership = SystemComponent;
};
struct Interactable {};
struct ScriptData {
    Json value = Json::object();
};
// Transform is the only spatial authority for rendering, physics and children.
struct Transform {
    using Ownership = SystemComponent;
    TransformPose local;
    TransformPose world; // Derived cache, written only by TransformSystem.
    Entity parent = 0;
    std::vector<Entity> children;
    std::type_index driver{typeid(void)}; // Exclusive local-pose writer, runtime only.
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
    std::type_index instanceDriver{typeid(void)};
};
struct DrawEntityID {
    using Ownership = SystemComponent;
    std::shared_ptr<const RenderTargetAsset> target;
};
// Local shape dimensions are authored here; PhysicsScene owns derived world shape,
// rigid pose and query state. Transform remains the spatial authority.
struct Collider {
    using Ownership = SystemComponent;
    BodyHandle body;
    ColliderShape shape; // Local authored shape; PhysicsScene contains its scaled world shape.
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
} // namespace whimsical
