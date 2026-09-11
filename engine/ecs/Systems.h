#pragma once
#include "Components.h"
#include "Changes.h"
#include "core/RenderScene.h"
#include "animation/AnimationCollision.h"
#include "navigation/Navigation.h"

namespace whimsical {
struct SceneStorage {
    SceneStorage() {
        registry.onStructure = [this](Entity e, std::type_index type) { changes.mark(e, type); };
    }
    Registry registry;
    PhysicsScene physics;
    RenderScene renderScene;
    AnimationCollision jointColliders{physics};
    Changes changes;
    std::function<void(Entity, std::type_index)> removeComponent;
    bool enabled(Entity e) const;
};
class RenderSystem {
    SceneStorage& s;
    void setInstancesAs(Entity, uint32_t, std::vector<ProxyTransform>, std::type_index);

  public:
    explicit RenderSystem(SceneStorage&);
    void add(Entity, RenderComponent, std::shared_ptr<const StaticMesh> = {});
    void set(Entity, RenderComponent, std::shared_ptr<const StaticMesh>);
    void remove(Entity);
    void publishTransform(Entity);
    void publishAttributes(Entity);
    void setMaterial(Entity, std::shared_ptr<const MaterialAsset>);
    void setVisible(Entity, bool);
    void setInstances(Entity, uint32_t count, std::vector<ProxyTransform>);
    void claimInstances(Entity, std::type_index);
    void releaseInstances(Entity, std::type_index);
    void setDrivenInstances(Entity, uint32_t, std::vector<ProxyTransform>, std::type_index);
    void setStaticMesh(Entity, std::shared_ptr<const StaticMesh>);
    void extract(Frame&, bool debug, RenderTargetAccess&);
};
class TransformSystem {
    SceneStorage& s;
    void propagate(Entity);
    void validateSubtree(Entity, const TransformPose&) const;
    void setLocalAs(Entity, const TransformPose&, Entity parent, std::type_index);

  public:
    explicit TransformSystem(SceneStorage& storage) : s(storage) {}
    void add(Entity, TransformPose = {});
    void remove(Entity);
    void setTransform(Entity, const PhysicsPose&); // World-space edit, converts to local.
    void setWorld(Entity, const TransformPose&);
    void setLocal(Entity, const TransformPose&);
    void setLocal(Entity, const TransformPose&, Entity parent);
    void setScale(Entity, vec3);
    void setParent(Entity, Entity parent, bool keepWorld = true);
    void claim(Entity, std::type_index);
    void release(Entity, std::type_index);
    void setDrivenLocal(Entity, const TransformPose&, std::type_index);
    void refreshEnabled(Entity);
};
class MotionSystem {
    SceneStorage& s;
    TransformSystem& transforms;
    NavigationSettings& navigation;
    vec3 characterPosition(Entity, vec3) const;

  public:
    MotionSystem(SceneStorage&, TransformSystem&, NavigationSettings&);
    void add(Entity, PhysicsBody);
    void remove(Entity);
    ColliderSettings collider(Entity) const;
    void set(Entity, PhysicsBody);
    void configureCollider(Entity, uint32_t layer, bool blocking, bool walkable, bool pickable);
    void setColliderShape(Entity, const ColliderShape&);
    void setSolid(Entity, bool);
    NavigationAgent agent(Entity) const;
    vec3 feet(Entity) const;
    vec3 moveCharacter(Entity, vec3);
    BodyMoveResult moveBody(Entity, vec3, quat, uint32_t mask = CollisionLayer::All);
    vec3 rootMotion(Entity, vec3, float);
    float setCharacterHeight(Entity, float);
    void bindRootMotion(Entity, RootMotionBinding);
    PhysicsPose solveRootMotion(Entity, const animation::Transform&, vec3 offset) const;
    std::vector<vec3> findPath(Entity, vec3) const;
};
class AnimationSystem {
    SceneStorage& s;
    MotionSystem& motion;
    TransformSystem& transforms;
    void attachAnimationInstance(Entity, std::unique_ptr<animation::Instance>, vec3);

  public:
    AnimationSystem(SceneStorage&, MotionSystem&, TransformSystem&);
    void attachAnimation(Entity, std::shared_ptr<const animation::Skeleton>,
                         std::unique_ptr<animation::Solver>, vec3 offset = vec3(0));
    void attachAnimation(Entity, const animation::Asset&, vec3 offset = vec3(0),
                         const animation::AttributeValues& attributes = {});
    void attachPrepared(Entity e, std::unique_ptr<animation::Instance> instance, vec3 offset,
                        AssetRef asset) {
        auto batch = Changes::Batch(s.changes);
        attachAnimationInstance(e, std::move(instance), offset);
        s.registry.get<Animator>(e).asset = std::move(asset);
        batch.commit();
    }
    void detachAnimation(Entity);
    void configureAnimation(Entity, vec3, const animation::AttributeValues&);
    void setAnimationAttribute(Entity, const std::string&, const std::string&);
    AnimationInspection inspectAnimation(Entity) const;
    void setSkinnedMesh(Entity, std::shared_ptr<const SkinnedMesh>);
    void removeSkin(Entity);
    void setAnimationInput(Entity, animation::Input);
    void resetAnimation(Entity);
    void setAnimationSolver(Entity, std::unique_ptr<animation::Solver>);
    const animation::Output& animationOutput(Entity) const;
    void update(float dt);
    void setAnimationJoints(Entity, std::vector<PhysicsPose>);
    void setAnimationJoints(Entity, std::vector<PhysicsPose>, std::shared_ptr<const animation::Skeleton>);
    void removeJoints(Entity);
    void removeJointColliders(Entity);
    std::vector<AnimationColliderDescription> describeColliders(Entity e) const {
        return s.jointColliders.describe(e);
    }
    void setAnimationColliders(Entity, const std::vector<AnimationColliderDescription>&);
    BodyHandle addAnimationCollider(Entity, uint32_t, const ColliderShape&, PhysicsPose, bool);
};
} // namespace whimsical
