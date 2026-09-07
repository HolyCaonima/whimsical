#pragma once
#include "Registry.h"
#include <functional>
#include <any>

namespace afterlight {
// Typed invalidations, not a second copy of component state. A system publishes
// after its authoritative write; subscribers read that committed state.
class Changes {
    using Key = std::pair<std::type_index, Entity>;
    std::map<std::type_index, std::vector<std::function<void(Entity)>>> subscribers;
    std::set<Key> pending;
    unsigned depth = 0;
    bool publishing = false;
    std::map<std::type_index, std::vector<std::function<void(const std::any&)>>> events;

  public:
    template <class T, class F> void subscribeEvent(F&& f) {
        events[typeid(T)].emplace_back(
            [f = std::forward<F>(f)](const std::any& value) { f(std::any_cast<const T&>(value)); });
    }
    template <class T> void emit(const T& value) {
        for (auto& f : events[typeid(T)])
            f(value);
    }
    template <class T, class F> void subscribe(F&& f) {
        subscribers[typeid(T)].emplace_back(std::forward<F>(f));
    }
    template <class T> void mark(Entity e) {
        mark(e, typeid(T));
    }
    void mark(Entity e, std::type_index type) {
        pending.emplace(type, e);
        if (!depth)
            flush();
    }
    void requireCommitted() const {
        if (depth || (!publishing && !pending.empty()))
            throw std::logic_error("Commit component changes before backend queries or extraction");
    }
    void flush() {
        if (publishing)
            return;
        publishing = true;
        try {
            while (!pending.empty()) {
                auto key = *pending.begin();
                pending.erase(pending.begin());
                for (auto& f : subscribers[key.first])
                    f(key.second);
            }
        } catch (...) {
            publishing = false;
            throw;
        }
        publishing = false;
    }
    // Explicit success boundary. Destruction only unwinds nesting; it never runs
    // fallible backend work while another exception is already in flight.
    class Batch {
        Changes& changes;
        bool finished = false;

      public:
        explicit Batch(Changes& c) : changes(c) {
            ++changes.depth;
        }
        Batch(const Batch&) = delete;
        ~Batch() {
            if (!finished)
                --changes.depth;
        }
        void commit() {
            finished = true;
            if (!--changes.depth)
                changes.flush();
        }
    };
};
struct WorldPoseChanged {};
struct EffectiveEnabledChanged {};
struct JointPoseChanged {};
struct RenderTransformChanged {};
struct RenderAttributesChanged {};
struct GeometryChanged {};
struct CharacterResized {
    Entity entity;
    float centerDelta;
};
} // namespace afterlight
