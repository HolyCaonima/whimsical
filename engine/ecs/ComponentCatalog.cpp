#include "ComponentCatalog.h"
#include "core/World.h"
#include <algorithm>

namespace afterlight {
ComponentCatalog& componentCatalog() {
    static ComponentCatalog catalog = [] {
        ComponentCatalog c;
        registerBuiltinComponents(c);
        return c;
    }();
    return catalog;
}
void ComponentCatalog::add(ComponentContract c) {
    if (types.count(c.name) || find(c.runtimeType))
        throw std::logic_error("Duplicate component contract: " + c.name);
    for (const auto& [name, type] : types)
        if (type.documentType == c.documentType)
            throw std::logic_error("Duplicate component document type");
    types.emplace(c.name, std::move(c));
}
const ComponentContract& ComponentCatalog::get(const std::string& name) const {
    auto it = types.find(name);
    if (it == types.end())
        throw std::invalid_argument("Unknown component: " + name);
    return it->second;
}
const ComponentContract* ComponentCatalog::find(std::type_index type) const {
    for (const auto& [name, c] : types)
        if (c.runtimeType == type)
            return &c;
    return nullptr;
}
const ComponentContract& ComponentCatalog::document(std::type_index type) const {
    for (const auto& [name, c] : types)
        if (c.documentType == type)
            return c;
    throw std::invalid_argument("Unregistered component document");
}
Json ComponentSet::json() const {
    Json result = Json::object();
    for (const auto& [name, value] : values)
        result[name] = componentCatalog().get(name).encode(value);
    return result;
}
ComponentSet ComponentSet::fromJson(const Json& json) {
    ComponentSet result;
    for (const auto& [name, value] : json.members())
        result.values.emplace(name, componentCatalog().get(name).decode(value));
    return result;
}
static std::vector<ComponentDependency> dependencies(const ComponentContract& c, const std::any& value) {
    auto result = c.dependencies;
    if (c.extraDependencies) {
        auto extra = c.extraDependencies(value);
        result.insert(result.end(), extra.begin(), extra.end());
    }
    return result;
}
void ComponentCatalog::validate(const ComponentSet& set) const {
    std::set<std::string> provided;
    for (const auto& [name, value] : set.values) {
        const auto& c = get(name);
        provided.insert(name);
        for (const auto& owned : c.owns) {
            if (set.contains(owned))
                throw std::invalid_argument(name + " owns derived " + owned);
            provided.insert(owned);
        }
    }
    for (const auto& [name, value] : set.values) {
        const auto& c = get(name);
        if (c.validateValue)
            c.validateValue(value);
        for (const auto& dep : dependencies(c, value))
            if (!provided.count(dep.name))
                throw std::invalid_argument(name + " requires " + dep.name);
        if (c.validate)
            c.validate(set, value);
    }
}
void ComponentCatalog::checkNative(const World& w, Entity e, std::type_index type, const void* value) const {
    w.registry().require(e);
    if (auto c = find(type)) {
        if (!c->nativeValue)
            throw std::logic_error("Use the component's owning system");
        auto combined = inspect(w, e);
        combined.values[c->name] = c->nativeValue(value);
        validate(combined);
    }
}
void ComponentCatalog::attach(World& w, Entity e, const ComponentSet& set, AssetManager& assets) const {
    w.registry().require(e);
    auto combined = inspect(w, e);
    for (const auto& [name, value] : set.values)
        combined.values[name] = value;
    validate(combined);
    std::set<std::string> available;
    for (const auto& [name, c] : types)
        if (c.present(w.registry(), e))
            available.insert(name);
    for (const auto& [name, value] : set.values) {
        const auto& c = get(name);
        if (c.validateValue)
            c.validateValue(value);
        if (available.count(name))
            throw std::logic_error("Component already present: " + name);
        for (const auto& owned : c.owns)
            if (available.count(owned) || set.contains(owned))
                throw std::logic_error(name + " owns derived " + owned);
    }
    std::vector<std::pair<const ComponentContract*, PreparedComponent>> plan;
    std::set<std::string> remaining;
    for (const auto& [name, value] : set.values)
        remaining.insert(name);
    // Topological preparation resolves every asset and checks input before any write.
    while (!remaining.empty()) {
        bool progressed = false;
        for (auto it = remaining.begin(); it != remaining.end();) {
            const auto& c = get(*it);
            const auto& value = set.values.at(*it);
            auto deps = dependencies(c, value);
            if (!std::all_of(deps.begin(), deps.end(),
                             [&](const auto& d) { return available.count(d.name); })) {
                ++it;
                continue;
            }
            if (c.validate)
                c.validate(combined, value);
            plan.emplace_back(&c, c.prepare(w, e, value, assets));
            available.insert(c.name);
            available.insert(c.owns.begin(), c.owns.end());
            it = remaining.erase(it);
            progressed = true;
        }
        if (!progressed)
            throw std::invalid_argument("Missing or cyclic component dependency: " + *remaining.begin());
    }
    auto batch = w.changes();
    ComponentAccess access(w, w.storage_);
    size_t installed = 0;
    try {
        for (auto& entry : plan) {
            ++installed;
            entry.second(access);
        }
    } catch (...) {
        // Only new capabilities are rolled back, in reverse dependency order.
        while (installed)
            remove(w, e, plan[--installed].first->name);
        batch.commit();
        throw;
    }
    batch.commit();
}
void ComponentCatalog::update(World& w, Entity e, const std::string& name, const std::any& value,
                              AssetManager& assets) const {
    w.registry().require(e);
    const auto& c = get(name);
    if (!c.present(w.registry(), e))
        throw std::logic_error("Component absent: " + name);
    auto combined = inspect(w, e);
    if (!combined.contains(name))
        throw std::logic_error("Component is derived: " + name);
    combined.values[name] = value;
    validate(combined);
    auto install = c.prepare(w, e, value, assets);
    auto batch = w.changes();
    ComponentAccess access(w, w.storage_);
    install(access);
    w.storage_.changes.mark(e, c.runtimeType);
    batch.commit();
}
static std::vector<ComponentDependency> runtimeDependencies(const ComponentContract& c, const World& w,
                                                            Entity e) {
    auto deps = c.dependencies;
    if (c.runtimeDependencies) {
        auto extra = c.runtimeDependencies(w, e);
        deps.insert(deps.end(), extra.begin(), extra.end());
    }
    return deps;
}
void ComponentCatalog::erasePlan(World& w, Entity e, std::set<std::string> removal) const {
    const auto& catalog = *this;
    bool changed;
    do {
        changed = false;
        auto current = removal;
        for (const auto& name : current)
            for (const auto& owned : catalog.get(name).owns)
                if (catalog.get(owned).present(w.registry(), e))
                    changed |= removal.insert(owned).second;
        for (const auto& [name, c] : catalog.entries()) {
            if (!c.present(w.registry(), e) || removal.count(name))
                continue;
            for (const auto& dep : runtimeDependencies(c, w, e))
                if (removal.count(dep.name) && dep.removal == OnDependencyRemoval::Cascade)
                    changed |= removal.insert(name).second;
        }
    } while (changed);
    for (const auto& [name, c] : catalog.entries()) {
        if (!c.present(w.registry(), e) || removal.count(name))
            continue;
        for (const auto& dep : runtimeDependencies(c, w, e))
            if (removal.count(dep.name))
                throw std::logic_error(name + " requires " + dep.name);
        for (const auto& owned : c.owns)
            if (removal.count(owned))
                throw std::logic_error(name + " owns " + owned);
    }
    std::vector<const ComponentContract*> plan;
    while (!removal.empty()) {
        bool progressed = false;
        for (auto it = removal.begin(); it != removal.end();) {
            bool needed = false;
            for (const auto& other : removal) {
                if (other == *it)
                    continue;
                for (const auto& dep : runtimeDependencies(catalog.get(other), w, e))
                    needed |= dep.name == *it;
                // Derived resources are released before their producer.
                const auto& owned = catalog.get(*it).owns;
                needed |= std::find(owned.begin(), owned.end(), other) != owned.end();
            }
            if (needed) {
                ++it;
                continue;
            }
            plan.push_back(&catalog.get(*it));
            it = removal.erase(it);
            progressed = true;
        }
        if (!progressed)
            throw std::logic_error("Cyclic component lifetime contract");
    }
    auto batch = w.changes();
    ComponentAccess access(w, w.storage_);
    for (auto c : plan) {
        if (c->erase)
            c->erase(access, e);
        else {
            w.storage_.registry.remove(e, c->runtimeType);
        }
    }
    batch.commit();
}
void ComponentCatalog::remove(World& w, Entity e, const std::string& name) const {
    w.registry().require(e);
    if (get(name).present(w.registry(), e))
        erasePlan(w, e, {name});
}
void ComponentCatalog::destroy(World& w, Entity e) const {
    std::set<std::string> names;
    for (const auto& [name, c] : types)
        if (c.present(w.registry(), e))
            names.insert(name);
    erasePlan(w, e, std::move(names));
}
ComponentSet ComponentCatalog::inspect(const World& w, Entity e) const {
    ComponentSet result;
    for (const auto& [name, c] : types)
        if (c.present(w.registry(), e))
            if (auto value = c.inspect(w, e))
                result.values.emplace(name, std::move(*value));
    return result;
}
ComponentSet ComponentCatalog::capture(const World& w, Entity e, const AssetManager& assets) const {
    auto result = inspect(w, e);
    for (auto& [name, value] : result.values)
        if (auto& resolve = get(name).resolveReferences)
            resolve(value, assets);
    return result;
}
} // namespace afterlight
