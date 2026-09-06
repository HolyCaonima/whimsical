#pragma once
#include "Types.h"
#include "RenderScene.h"
#include "physics/PhysicsScene.h"
#include "animation/AnimationCollision.h"
#include "navigation/Navigation.h"
namespace afterlight {
struct RenderComponent {
    Shape shape = Shape::Box;
    vec3 scale{1}, offset{0}, animationScale{1};
    uint32_t material = 0;
    bool visible = true;
};
struct GameObject {
    uint32_t id = 0;
    std::string name;
    vec3 position{0};
    float yaw = 0;
    RenderComponent render;
    BodyHandle physical;
    uint32_t proxy = 0; // Stable render slot, owned by the render scene.
    bool interactable = false, enabled = true, alive = true;
    std::vector<PhysicsPose> joints;
};
class World {
    std::vector<GameObject> objects_;
    PhysicsScene physics_;
    RenderScene scene_;
    AnimationCollision animationCollision_{physics_};
    GameObject& mutableObject(uint32_t id);
    void syncPose(GameObject&);
    // The two funnels every render-visible mutation goes through. Keeping the derivation
    // of proxy state in one place each is what makes it impossible to change an object
    // and silently forget to tell the renderer.
    static ProxyTransform proxyTransform(const GameObject&);
    void publishTransform(const GameObject&);
    void publishAttributes(const GameObject&);

  public:
    std::vector<Material> materials;
    std::vector<Light> lights;
    Camera camera;
    NavigationSettings navigation;
    uint32_t playerId = 0, selected = 0, hovered = 0;
    std::string state = "Idle", message = "The Rain Court";
    vec3 destination{0};
    bool hasDestination = false, resetHistory = true;
    std::vector<vec3> path;
    const GameObject& entity(uint32_t id) const;
    const std::vector<GameObject>& objects() const {
        return objects_;
    }
    const PhysicsScene& physics() const {
        return physics_;
    }
    const RenderScene& renderScene() const {
        return scene_;
    }
    uint32_t spawn(std::string name, Shape, vec3 position, vec3 scale, uint32_t material, bool blocking,
                   bool interactable);
    void destroy(uint32_t);
    void setEnabled(uint32_t, bool);
    void setVisible(uint32_t, bool);
    void setPose(uint32_t, vec3 position, float yaw, float renderHeight);
    void setVisualPose(uint32_t, vec3 offset, vec3 scale);
    void setMaterial(uint32_t, uint32_t);
    void setSolid(uint32_t, bool);
    void configureCollider(uint32_t, uint32_t layer, bool blocking, bool walkable, bool pickable);
    void setColliderShape(uint32_t, const ColliderShape&);
    NavigationAgent agent(uint32_t) const;
    vec3 feet(uint32_t) const;
    vec3 moveCharacter(uint32_t, vec3 delta);
    vec3 rootMotion(uint32_t, vec3 localDelta, float deltaYaw);
    float setCharacterHeight(uint32_t, float height);
    BodyHandle addAnimationCollider(uint32_t, uint32_t joint, const ColliderShape&, PhysicsPose local,
                                    bool blocking);
    void setAnimationJoints(uint32_t, std::vector<PhysicsPose>);
    std::vector<vec3> findPath(uint32_t, vec3 target) const;
    std::optional<vec3> groundAt(float x, float y, const Input&) const;
    uint32_t pick(float x, float y, const Input&) const;
    Frame snapshot(const Input&, uint64_t tick, double time, int debugView, bool physicsDebug = false);
};
} // namespace afterlight
