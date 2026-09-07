#pragma once
#include "Components.h"
#include "core/RenderScene.h"
#include "animation/AnimationCollision.h"
#include "navigation/Navigation.h"

namespace afterlight {
struct SceneStorage {
    Registry registry;
    PhysicsScene physics;
    RenderScene renderScene;
    AnimationCollision jointColliders{physics};
    bool enabled(Entity e) const;
};
class RenderSystem {
    SceneStorage& s;
    const std::vector<Material>& materials;

  public:
    RenderSystem(SceneStorage& storage, const std::vector<Material>& m) : s(storage), materials(m) {}
    void add(Entity, RenderComponent, std::shared_ptr<const StaticMesh> = {});
    void remove(Entity);
    void publishTransform(Entity);
    void publishAttributes(Entity);
    void setVisualPose(Entity, vec3 offset, vec3 scale);
    void setScale(Entity, vec3);
    void setMaterial(Entity, uint32_t);
    void setVisible(Entity, bool);
    void setStaticMesh(Entity, std::shared_ptr<const StaticMesh>);
    void extract(Frame&, bool debug);
};
class TransformSystem {
    SceneStorage& s;
    RenderSystem& render;
    void propagate(Entity);

  public:
    TransformSystem(SceneStorage& storage, RenderSystem& r) : s(storage), render(r) {}
    void add(Entity, PhysicsPose = {});
    void remove(Entity);
    void setTransform(Entity, const PhysicsPose&); // World-space edit, converts to local.
    void setLocal(Entity, const PhysicsPose&);
    void setParent(Entity, Entity parent, bool keepWorld = true);
    void refreshEnabled(Entity);
};
class MotionSystem {
    SceneStorage& s;
    TransformSystem& transforms;
    NavigationSettings& navigation;

  public:
    MotionSystem(SceneStorage& storage, TransformSystem& t, NavigationSettings& n)
        : s(storage), transforms(t), navigation(n) {}
    void add(Entity, PhysicsBody);
    void remove(Entity);
    void configureCollider(Entity, uint32_t layer, bool blocking, bool walkable, bool pickable);
    void setColliderShape(Entity, const ColliderShape&);
    void setSolid(Entity, bool);
    NavigationAgent agent(Entity) const;
    vec3 feet(Entity) const;
    vec3 moveCharacter(Entity, vec3);
    BodyMoveResult moveBody(Entity, vec3, quat, uint32_t mask = CollisionLayer::All);
    vec3 rootMotion(Entity, vec3, float);
    float setCharacterHeight(Entity, float);
    std::vector<vec3> findPath(Entity, vec3) const;
};
class AnimationSystem {
    SceneStorage& s;
    MotionSystem& motion;
    void attachAnimationInstance(Entity, std::unique_ptr<animation::Instance>, bool, vec3);

  public:
    AnimationSystem(SceneStorage& storage, MotionSystem& m) : s(storage), motion(m) {}
    void attachAnimation(Entity, std::shared_ptr<const animation::Skeleton>,
                         std::unique_ptr<animation::Solver>, bool rootMotion = true, vec3 offset = vec3(0));
    void attachAnimation(Entity, const animation::Asset&, bool rootMotion = true, vec3 offset = vec3(0),
                         const animation::AttributeValues& attributes = {});
    void detachAnimation(Entity);
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
    void removeJoints(Entity);
    void removeJointColliders(Entity);
    BodyHandle addAnimationCollider(Entity, uint32_t, const ColliderShape&, PhysicsPose, bool);
};
} // namespace afterlight
