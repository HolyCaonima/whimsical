#pragma once
#include "Types.h"
#include "RenderScene.h"
#include "physics/PhysicsScene.h"
#include "animation/AnimationCollision.h"
#include "animation/Animation.h"
#include "animation/SkinnedMesh.h"
#include <map>
#include "assets/Asset.h"
#include "navigation/Navigation.h"
namespace afterlight {
struct GameObject {
    uint32_t id = 0;
    std::string name, persistentId;
    vec3 position{0};
    float yaw = 0;
    RenderComponent render;
    BodyHandle physical;
    uint32_t proxy = 0; // Stable render slot, owned by the render scene.
    bool interactable = false, enabled = true, alive = true;
    std::vector<PhysicsPose> joints;
};
class World {
    friend class ScenePersistence;
    AssetRef mapAsset_;
    std::map<std::string, uint32_t> objectIds_;
    struct AnimatedObject {
        std::unique_ptr<animation::Instance> instance;
        animation::Input input;
        std::optional<AssetRef> asset;
        bool applyRootMotion = true;
        vec3 rootOffset{0}; // Skeleton origin relative to physical object's centre.
        std::shared_ptr<const SkinnedMesh> mesh;
        std::vector<unsigned> meshJoints;
    };
    std::map<uint32_t, AnimatedObject> animations_;
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
    void attachAnimationInstance(uint32_t, std::unique_ptr<animation::Instance>, bool, vec3);

  public:
    std::vector<AssetRef> sceneScripts;
    Json sceneData = Json::object();
    std::map<std::string, std::string> sceneReferences;
    ObjectPath objectPath(uint32_t) const;
    uint32_t resolveObject(const ObjectPath&) const;
    uint32_t findObject(const std::string& persistentId) const;
    const AssetRef& mapAsset() const {
        return mapAsset_;
    }
    void clearScene();
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
                   bool interactable, std::string persistentId = {});
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
    void attachAnimation(uint32_t, std::shared_ptr<const animation::Skeleton>,
                         std::unique_ptr<animation::Solver>, bool applyRootMotion = true,
                         vec3 rootOffset = {0, 0, 0});
    void detachAnimation(uint32_t);
    void attachAnimation(uint32_t, const animation::Asset&, bool applyRootMotion = true,
                         vec3 rootOffset = {0, 0, 0});
    void setAnimationAttribute(uint32_t, const std::string& key, const std::string& value);
    AnimationInspection inspectAnimation(uint32_t) const;
    void setSkinnedMesh(uint32_t, std::shared_ptr<const SkinnedMesh>);
    void setAnimationInput(uint32_t, animation::Input);
    void resetAnimation(uint32_t); // Teleport/reinitialization resets sequence and IK history.
    void setAnimationSolver(uint32_t, std::unique_ptr<animation::Solver>);
    const animation::Output& animationOutput(uint32_t) const;
    void updateAnimations(float dt);
    std::vector<vec3> findPath(uint32_t, vec3 target) const;
    std::optional<vec3> groundAt(float x, float y, const Input&) const;
    uint32_t pick(float x, float y, const Input&) const;
    Frame snapshot(const Input&, uint64_t tick, double time, int debugView, bool physicsDebug = false);
};
} // namespace afterlight
