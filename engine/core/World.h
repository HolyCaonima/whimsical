#pragma once
#include "ecs/Systems.h"
#include "assets/MaterialAsset.h"
#include <type_traits>
namespace afterlight {
struct SceneResources {
    AssetRef mapAsset;
    std::vector<AssetRef> scripts;
    Json data = Json::object();
    std::map<std::string, std::string> references;
    std::vector<Material> materials;
    std::map<uint32_t, std::shared_ptr<const MaterialAsset>> materialAssets;
    std::vector<Light> lights;
    Camera camera;
    NavigationSettings navigation;
};
struct GameplayState {
    Entity playerId = 0, selected = 0, hovered = 0;
    std::string state = "Idle", message = "The Rain Court";
    vec3 destination{0};
    bool hasDestination = false;
    std::vector<vec3> path;
};
// Composition root and scene lifecycle. Domain operations belong to systems.
class World {
    friend class ScenePersistence;
    SceneStorage storage_;
    std::map<std::string, Entity> objectIds_;

  public:
    SceneResources resources;
    GameplayState gameplay;
    bool resetHistory = true;
    RenderSystem render{storage_, resources.materials};
    TransformSystem transforms{storage_, render};
    MotionSystem motion{storage_, transforms, resources.navigation};
    AnimationSystem animation{storage_, motion};
    const Registry& registry() const {
        return storage_.registry;
    }
    template <class T> const T& get(Entity e) const {
        return registry().get<T>(e);
    }
    template <class T> bool has(Entity e) const {
        return registry().has<T>(e);
    }
    template <class T, class F> void edit(Entity e, F&& edit) {
        static_assert(!std::is_same_v<T, Transform> && !std::is_same_v<T, Collider> &&
                          !std::is_same_v<T, Renderable> && !std::is_same_v<T, Animator> &&
                          !std::is_same_v<T, Skin> && !std::is_same_v<T, JointPose> &&
                          !std::is_same_v<T, JointColliders> && !std::is_same_v<T, Identity> &&
                          !std::is_same_v<T, Disabled>,
                      "Use the component's owning system");
        edit(storage_.registry.get<T>(e));
    }
    template <class T, class... Args> void add(Entity e, Args&&... args) {
        static_assert(!std::is_same_v<T, Transform> && !std::is_same_v<T, Collider> &&
                          !std::is_same_v<T, Renderable> && !std::is_same_v<T, Animator> &&
                          !std::is_same_v<T, Skin> && !std::is_same_v<T, JointPose> &&
                          !std::is_same_v<T, JointColliders> && !std::is_same_v<T, Identity> &&
                          !std::is_same_v<T, Disabled>,
                      "Use the component's owning system");
        storage_.registry.emplace<T>(e, std::forward<Args>(args)...);
        if constexpr (std::is_same_v<T, Interactable>)
            render.publishAttributes(e);
    }
    template <class T> void remove(Entity e) {
        if constexpr (std::is_same_v<T, Transform>)
            transforms.remove(e);
        else if constexpr (std::is_same_v<T, Collider>)
            motion.remove(e);
        else if constexpr (std::is_same_v<T, Renderable>)
            render.remove(e);
        else if constexpr (std::is_same_v<T, Animator>)
            animation.detachAnimation(e);
        else if constexpr (std::is_same_v<T, Skin>)
            animation.removeSkin(e);
        else if constexpr (std::is_same_v<T, JointPose>)
            animation.removeJoints(e);
        else if constexpr (std::is_same_v<T, JointColliders>)
            animation.removeJointColliders(e);
        else {
            static_assert(!std::is_same_v<T, Identity> && !std::is_same_v<T, Disabled>,
                          "Use entity lifecycle API");
            storage_.registry.remove<T>(e);
            if constexpr (std::is_same_v<T, Interactable>)
                render.publishAttributes(e);
        }
    }
    Entity create(std::string name = {}, std::string persistentId = {});
    void destroy(Entity); // Hierarchy subtree, children first.
    void setEnabled(Entity, bool);
    bool enabled(Entity e) const {
        registry().require(e);
        return storage_.enabled(e);
    }
    void clearScene();
    ObjectPath objectPath(Entity) const;
    Entity resolveObject(const ObjectPath&) const;
    Entity findObject(const std::string&) const;
    const AssetRef& mapAsset() const {
        return resources.mapAsset;
    }
    const PhysicsScene& physics() const {
        return storage_.physics;
    }
    const RenderScene& renderScene() const {
        return storage_.renderScene;
    }
    // Script/input/motion -> animation/root motion -> joint colliders. Transform edits
    // propagate synchronously before queries; extraction never advances simulation.
    void update(float dt) {
        animation.update(dt);
    }
    std::optional<vec3> groundAt(float, float, const Input&) const;
    Entity pick(float, float, const Input&) const;
    Frame snapshot(const Input&, uint64_t, double, int, bool physicsDebug = false);
};
} // namespace afterlight
