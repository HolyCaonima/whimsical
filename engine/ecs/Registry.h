#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <typeindex>
#include <vector>
#include <functional>

namespace whimsical {
using Entity = uint32_t;
// IDs are never reused, including across map loads. Only live entities occupy storage.
// Ordered pools keep references stable and make simulation/serialization deterministic.
// Queries return IDs, so callers may safely compose or destroy entities while iterating.
class Registry {
    struct Storage {
        virtual ~Storage() = default;
        virtual void erase(Entity) = 0;
        virtual bool contains(Entity) const = 0;
    };
    template <class T> struct Pool : Storage {
        std::map<Entity, T> values;
        void erase(Entity e) override {
            values.erase(e);
        }
        bool contains(Entity e) const override {
            return values.count(e) != 0;
        }
    };
    std::map<std::type_index, std::unique_ptr<Storage>> pools_;
    std::set<Entity> live_;
    Entity next_ = 1;
    template <class T> Pool<T>& pool() {
        auto& p = pools_[typeid(T)];
        if (!p)
            p = std::make_unique<Pool<T>>();
        return static_cast<Pool<T>&>(*p);
    }
    template <class T> const Pool<T>* pool() const {
        auto p = pools_.find(typeid(T));
        return p == pools_.end() ? nullptr : static_cast<const Pool<T>*>(p->second.get());
    }

  public:
    std::function<void(Entity, std::type_index)> onStructure;
    void continueIdentitySequence(const Registry& previous) {
        next_ = previous.next_;
    }
    void exchangeScene(Registry& other) {
        pools_.swap(other.pools_);
        live_.swap(other.live_);
        std::swap(next_, other.next_);
    }
    Entity create() {
        if (!next_)
            throw std::overflow_error("Entity identity space exhausted");
        Entity e = next_++;
        live_.insert(e);
        return e;
    }
    bool contains(Entity e) const {
        return live_.count(e) != 0;
    }
    void require(Entity e) const {
        if (!contains(e))
            throw std::out_of_range("Invalid entity");
    }
    void destroy(Entity e) {
        require(e);
        for (auto& p : pools_)
            if (p.second->contains(e)) {
                p.second->erase(e);
                if (onStructure)
                    onStructure(e, p.first);
            }
        live_.erase(e);
    }
    template <class T, class... Args> T& emplace(Entity e, Args&&... args) {
        require(e);
        auto result = pool<T>().values.try_emplace(e, std::forward<Args>(args)...);
        if (!result.second)
            throw std::logic_error("Component already present");
        if (onStructure)
            onStructure(e, typeid(T));
        return result.first->second;
    }
    template <class T> const T* tryGet(Entity e) const {
        auto p = pool<T>();
        if (!p)
            return nullptr;
        auto v = p->values.find(e);
        return v == p->values.end() ? nullptr : &v->second;
    }
    template <class T> T* tryGet(Entity e) {
        return const_cast<T*>(static_cast<const Registry&>(*this).tryGet<T>(e));
    }
    template <class T> bool has(Entity e) const {
        return tryGet<T>(e) != nullptr;
    }
    template <class T> const T& get(Entity e) const {
        auto p = tryGet<T>(e);
        if (!p)
            throw std::out_of_range("Entity lacks requested component");
        return *p;
    }
    template <class T> T& get(Entity e) {
        return const_cast<T&>(static_cast<const Registry&>(*this).get<T>(e));
    }
    template <class T> void remove(Entity e) {
        remove(e, typeid(T));
    }
    void remove(Entity e, std::type_index type) {
        require(e);
        auto p = pools_.find(type);
        if (p != pools_.end() && p->second->contains(e)) {
            p->second->erase(e);
            if (onStructure)
                onStructure(e, type);
        }
    }
    template <class First, class... Rest> std::vector<Entity> view() const {
        std::vector<Entity> result;
        if (auto p = pool<First>())
            for (const auto& v : p->values)
                if ((has<Rest>(v.first) && ...))
                    result.push_back(v.first);
        return result;
    }
    std::vector<Entity> entities() const {
        return {live_.begin(), live_.end()};
    }
    size_t size() const {
        return live_.size();
    }
};
} // namespace whimsical
