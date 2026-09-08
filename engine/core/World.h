#pragma once
#include "ecs/Systems.h"
#include "assets/MaterialAsset.h"
#include "ecs/ComponentCatalog.h"
#include <type_traits>
namespace afterlight {
template <class T, class = void> struct SystemOwned : std::false_type {};
template <class T> struct SystemOwned<T, std::void_t<typename T::Ownership>> : std::true_type {};
struct SceneResources {
    AssetRef mapAsset;
    std::vector<AssetRef> scripts;
    Json data = Json::object();
    std::map<std::string, std::string> references;
    std::vector<Material> materials;
    std::map<uint32_t, std::shared_ptr<const MaterialAsset>> materialAssets;
    std::set<uint32_t> transientMaterials;
    Camera camera;
    NavigationSettings navigation;
};
// Composition root and scene lifecycle. Domain operations belong to systems.
class World {
    friend class ScenePersistence;
    friend class ComponentCatalog;
    SceneStorage storage_;
    std::map<std::string, Entity> objectIds_;

  public:
    World();
    ~World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    Changes::Batch changes() {
        return Changes::Batch(storage_.changes);
    }
    Changes::Notifications deferObservers() {
        return Changes::Notifications(storage_.changes);
    }
    void commitChanges() {
        storage_.changes.flush();
    }
    template <class T, class F> void onChange(F&& f) {
        storage_.changes.observe<T>(std::forward<F>(f));
    }
    SceneResources resources;
    RenderTargetAccess renderTargets;
    bool resetHistory = true;
    RenderSystem render{storage_, resources.materials};
    TransformSystem transforms{storage_};
    MotionSystem motion{storage_, transforms, resources.navigation};
    AnimationSystem animation{storage_, motion, transforms};
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
        static_assert(!SystemOwned<T>::value, "Use the component's owning system");
        auto batch = changes();
        T draft = get<T>(e);
        edit(draft);
        componentCatalog().checkNative(*this, e, typeid(T), &draft);
        storage_.registry.get<T>(e) = std::move(draft);
        storage_.changes.mark<T>(e);
        batch.commit();
    }
    template <class T, class... Args> void add(Entity e, Args&&... args) {
        static_assert(!SystemOwned<T>::value, "Use the component's owning system");
        auto batch = changes();
        T value{std::forward<Args>(args)...};
        componentCatalog().checkNative(*this, e, typeid(T), &value);
        storage_.registry.emplace<T>(e, std::move(value));
        batch.commit();
    }
    template <class T> void remove(Entity e) {
        static_assert(!std::is_same_v<T, Identity> && !std::is_same_v<T, Disabled>,
                      "Use entity lifecycle API");
        if (auto c = componentCatalog().find(typeid(T)))
            componentCatalog().remove(*this, e, c->name);
        else {
            auto batch = changes();
            storage_.registry.remove<T>(e);
            batch.commit();
        }
    }
    template <class T> void set(Entity e, T value, AssetManager& assets) {
        componentCatalog().update(*this, e, componentCatalog().document(typeid(T)).name, value, assets);
    }
    Entity create(std::string name = {}, std::string persistentId = {}, bool persistent = true);
    void rename(Entity, std::string);
    void setPersistent(Entity, bool);
    bool persistent(Entity) const;
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
        storage_.changes.requireCommitted();
        return storage_.physics;
    }
    const RenderScene& renderScene() const {
        storage_.changes.requireCommitted();
        return storage_.renderScene;
    }
    // Script/input/motion -> animation/root motion -> joint colliders. Transform edits
    // propagate synchronously before queries; extraction never advances simulation.
    void update(float dt) {
        animation.update(dt);
    }
    std::optional<vec3> groundAt(float, float, const Input&, const Camera* = nullptr) const;
    Entity pick(float, float, const Input&, const Camera* = nullptr) const;
    Frame snapshot(const Input&, uint64_t, double, int, bool physicsDebug = false,
                   const RenderView* view = nullptr);
};
} // namespace afterlight
