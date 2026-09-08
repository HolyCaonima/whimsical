#pragma once
#include "Registry.h"
#include "core/Math.h"
#include <functional>
#include <any>
#include <exception>

namespace afterlight {
// Typed invalidations, not a second copy of component state. A system publishes
// after its authoritative write; subscribers read that committed state.
class Changes {
    using Key = std::pair<std::type_index, Entity>;
    std::map<std::type_index, std::vector<std::function<void(Entity)>>> subscribers;
    std::map<std::type_index, std::vector<std::function<void(Entity)>>> observers;
    std::set<Key> pending, notifications;
    unsigned depth = 0, notificationDepth = 0;
    bool publishing = false, synchronizing = false, observing = false;
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
    template <class T, class F> void observe(F&& f) {
        observers[typeid(T)].emplace_back(std::forward<F>(f));
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
    void requireWritable() const {
        if (observing)
            throw std::logic_error("Component observers are read-only");
    }
    void requireCommitted() const {
        if (depth || synchronizing || !pending.empty())
            throw std::logic_error("Commit component changes before backend queries or extraction");
    }
    // A rolled-back creation has no externally observable lifetime.
    void forget(Entity e) {
        for (auto* queue : {&pending, &notifications})
            for (auto it = queue->begin(); it != queue->end();)
                if (it->second == e)
                    it = queue->erase(it);
                else
                    ++it;
    }
    void flush() {
        if (depth)
            throw std::logic_error("Commit the enclosing component batch first");
        if (publishing)
            return;
        publishing = true;
        std::exception_ptr observerFailure;
        try {
            while (!pending.empty() || !notifications.empty()) {
                synchronizing = true;
                while (!pending.empty()) {
                    auto key = *pending.begin();
                    pending.erase(pending.begin());
                    try {
                        for (auto& f : subscribers[key.first])
                            f(key.second);
                    } catch (...) {
                        pending.insert(key); // Idempotent derived synchronization is retryable.
                        throw;
                    }
                    notifications.insert(key);
                }
                synchronizing = false;
                if (notificationDepth)
                    break;
                auto ready = std::move(notifications);
                notifications.clear();
                observing = true;
                for (const auto& key : ready)
                    for (auto& f : observers[key.first])
                        try {
                            f(key.second);
                        } catch (...) {
                            if (!observerFailure)
                                observerFailure = std::current_exception();
                        }
                observing = false;
            }
        } catch (...) {
            observing = synchronizing = publishing = false;
            throw;
        }
        publishing = false;
        // Observer errors never roll back committed state or replay callbacks.
        if (observerFailure)
            std::rethrow_exception(observerFailure);
    }
    // Scene/realm replacement may synchronize backends and run initialization
    // while delaying external observation until both owners are ready.
    class Notifications {
        Changes& changes;
        bool finished = false;

      public:
        explicit Notifications(Changes& c) : changes(c) {
            changes.requireWritable();
            ++changes.notificationDepth;
        }
        Notifications(const Notifications&) = delete;
        ~Notifications() {
            if (!finished)
                --changes.notificationDepth;
        }
        void commit() {
            if (finished)
                throw std::logic_error("Notifications already committed");
            finished = true;
            if (!--changes.notificationDepth)
                changes.flush();
        }
    };
    // Explicit success boundary. Destruction only unwinds nesting; it never runs
    // fallible backend work while another exception is already in flight.
    class Batch {
        Changes& changes;
        bool finished = false;

      public:
        explicit Batch(Changes& c) : changes(c) {
            changes.requireWritable();
            ++changes.depth;
        }
        Batch(const Batch&) = delete;
        ~Batch() {
            if (!finished)
                --changes.depth;
        }
        void commit() {
            if (finished)
                throw std::logic_error("Component batch already committed");
            finished = true;
            if (!--changes.depth)
                changes.flush();
        }
    };
};
struct WorldPoseChanged {};
struct EffectiveEnabledChanged {};
struct JointPoseChanged {};
struct RenderAttributesChanged {};
struct GeometryChanged {};
struct CharacterResized {
    Entity entity;
    vec3 centerDelta;
};
} // namespace afterlight
