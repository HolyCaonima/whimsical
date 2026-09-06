#pragma once
#include "core/Math.h"
#include "DynamicAabbTree.h"
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace afterlight {
struct BodyHandle {
    uint32_t slot = UINT32_MAX, generation = 0;
    explicit operator bool() const {
        return slot != UINT32_MAX;
    }
    bool operator==(BodyHandle b) const {
        return slot == b.slot && generation == b.generation;
    }
};
enum class ColliderType { Box, Capsule };
enum class BodyMotion { Static, Kinematic };
namespace CollisionLayer {
constexpr uint32_t World = 1, Character = 2, Trigger = 4, All = 0xffffffffu;
}
struct PhysicsPose {
    vec3 position{0};
    quat rotation{1, 0, 0, 0};
};
struct ColliderShape {
    ColliderType type = ColliderType::Box;
    vec3 halfExtents{.5f};
    float radius = .4f, halfSegment = .6f;
    static ColliderShape box(vec3 halfExtents);
    static ColliderShape capsule(float radius, float height);
    float height() const {
        return 2 * (radius + halfSegment);
    }
};
struct PhysicsBody {
    uint32_t owner = 0;
    ColliderShape shape;
    PhysicsPose pose;
    BodyMotion motion = BodyMotion::Static;
    uint32_t layer = CollisionLayer::World;
    bool enabled = true, blocking = true, walkable = false, pickable = true;
};
struct QueryFilter {
    uint32_t mask = CollisionLayer::All, ignoreOwner = 0;
    bool blockingOnly = false, walkableOnly = false, pickableOnly = false;
};
struct PhysicsHit {
    BodyHandle body;
    uint32_t owner = 0;
    vec3 position{0}, normal{0};
    float distance = 0, fraction = 0;
};
struct CapsuleQuery {
    vec3 center{0};
    float radius = .4f, height = 2;
};
struct MoveResult {
    vec3 position{0}, applied{0};
    bool blocked = false;
    std::vector<PhysicsHit> contacts;
};
// A rigid query shape may translate and rotate; it is independent of character grounding.
struct ShapeQuery {
    ColliderShape shape;
    PhysicsPose pose;
};
struct BodyMoveResult {
    PhysicsPose pose;
    vec3 applied{0};
    bool blocked = false;
    std::vector<PhysicsHit> contacts;
};

// Main-thread spatial authority. No render objects, meshes, materials, Vulkan or World dependencies.
// This backend implements scene queries and kinematic motion, not force-driven rigid-body dynamics.
class PhysicsScene {
    struct Slot {
        PhysicsBody body;
        PhysicsBounds bounds;
        uint32_t generation = 1;
        bool alive = false;
        int leaf = -1;
    };
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    std::thread::id owner_ = std::this_thread::get_id();
    uint64_t revision_ = 0;
    DynamicAabbTree broadphase_;
    void checkThread() const;
    Slot& require(BodyHandle);
    const Slot& require(BodyHandle) const;
    static bool accepts(const PhysicsBody&, QueryFilter);

  public:
    PhysicsScene() = default;
    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;
    BodyHandle create(const PhysicsBody&);
    void destroy(BodyHandle);
    bool contains(BodyHandle) const;
    const PhysicsBody& body(BodyHandle) const;
    void setPose(BodyHandle, const PhysicsPose&);
    void setShape(BodyHandle, const ColliderShape&);
    void setMotion(BodyHandle, BodyMotion);
    void setProperties(BodyHandle, uint32_t layer, bool blocking, bool walkable, bool pickable);
    void setEnabled(BodyHandle, bool);
    uint64_t revision() const;
    size_t size() const;
    PhysicsBounds bounds(BodyHandle) const;
    void rebuildBroadphase();
    BroadphaseStatistics broadphaseStatistics() const;
    std::vector<BodyHandle> bodies(QueryFilter = {}) const;
    std::optional<PhysicsHit> raycast(vec3 origin, vec3 direction, float maxDistance, QueryFilter = {}) const;
    std::vector<PhysicsHit> overlapCapsule(const CapsuleQuery&, QueryFilter = {}) const;
    std::optional<PhysicsHit> sweepCapsule(const CapsuleQuery&, vec3 delta, QueryFilter = {}) const;
    MoveResult moveAndSlide(const CapsuleQuery&, vec3 delta, QueryFilter = {}) const;
    std::vector<PhysicsHit> overlapShape(const ShapeQuery&, QueryFilter = {}) const;
    std::optional<PhysicsHit> sweepShape(const ShapeQuery&, vec3 delta, quat targetRotation,
                                        QueryFilter = {}) const;
    BodyMoveResult moveAndSlide(const ShapeQuery&, vec3 delta, quat targetRotation,
                                QueryFilter = {}) const;
    // Keep feet fixed; reject expansion into ceilings or other bodies.
    bool resizeCharacter(BodyHandle, float height);
};
} // namespace afterlight
