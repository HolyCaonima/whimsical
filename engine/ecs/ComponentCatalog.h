#pragma once
#include "Registry.h"
#include "assets/AssetManager.h"
#include <any>
#include <functional>
#include <optional>
#include <utility>

namespace afterlight {
class World;
struct SceneStorage;
// Mutable access exists only while the catalog executes a component lifecycle
// hook. Extensible backends need no friendship or escape hatch on World.
class ComponentAccess {
    friend class ComponentCatalog;
    ComponentAccess(World& w, SceneStorage& s) : world(w), storage(s) {}

  public:
    World& world;
    SceneStorage& storage;
};
using PreparedComponent = std::function<void(ComponentAccess&)>;
struct ComponentSet;
enum class OnDependencyRemoval { Reject, Cascade };
struct ComponentDependency {
    std::string name;
    OnDependencyRemoval removal = OnDependencyRemoval::Reject;
};
// A component's authored format, composition and lifetime are declared together.
// prepare must not mutate World. Its returned installer owns resolved assets.
struct ComponentContract {
    std::string name;
    std::type_index runtimeType{typeid(void)}, documentType{typeid(void)};
    std::vector<ComponentDependency> dependencies;
    std::vector<std::string> owns;
    std::function<bool(const Registry&, Entity)> present;
    std::function<std::any(const Json&)> decode;
    std::function<Json(const std::any&)> encode;
    std::function<std::vector<ComponentDependency>(const std::any&)> extraDependencies;
    std::function<std::vector<ComponentDependency>(const World&, Entity)> runtimeDependencies;
    std::function<void(const ComponentSet&, const std::any&)> validate;
    std::function<void(const std::any&)> validateValue;
    std::function<PreparedComponent(const World&, Entity, const std::any&, AssetManager&)> prepare;
    std::function<std::optional<std::any>(const World&, Entity, const AssetManager&)> capture;
    std::function<void(ComponentAccess&, Entity)> erase;
    std::function<void(const void*)> validateRuntime;
};
class ComponentCatalog {
    std::map<std::string, ComponentContract> types;
    void erasePlan(World&, Entity, std::set<std::string>) const;

  public:
    void add(ComponentContract);
    const ComponentContract& get(const std::string&) const;
    const ComponentContract* find(std::type_index) const;
    const ComponentContract& document(std::type_index) const;
    const auto& entries() const {
        return types;
    }
    void validate(const ComponentSet&) const;
    void checkNativeAdd(const World&, Entity, std::type_index, const void*) const;
    void attach(World&, Entity, const ComponentSet&, AssetManager&) const;
    void remove(World&, Entity, const std::string&) const;
    void destroy(World&, Entity) const;
    ComponentSet capture(const World&, Entity, const AssetManager&) const;
};
ComponentCatalog& componentCatalog();
struct ComponentSet {
    std::map<std::string, std::any> values;
    bool contains(const std::string& name) const {
        return values.count(name) != 0;
    }
    template <class T> const T* find() const {
        auto p = values.find(componentCatalog().document(typeid(T)).name);
        return p == values.end() ? nullptr : &std::any_cast<const T&>(p->second);
    }
    template <class T> T* find() {
        return const_cast<T*>(std::as_const(*this).find<T>());
    }
    template <class T> void set(T value) {
        values[componentCatalog().document(typeid(T)).name] = std::move(value);
    }
    Json json() const;
    static ComponentSet fromJson(const Json&);
};
template <class Runtime, class Document> ComponentContract component(const char* name) {
    ComponentContract c;
    c.name = name;
    c.runtimeType = typeid(Runtime);
    c.documentType = typeid(Document);
    c.present = [](const Registry& r, Entity e) { return r.has<Runtime>(e); };
    return c;
}
void registerBuiltinComponents(ComponentCatalog&);
} // namespace afterlight
