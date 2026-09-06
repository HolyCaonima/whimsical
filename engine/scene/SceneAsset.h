#pragma once
#include "assets/Asset.h"
#include "core/Types.h"
#include "physics/PhysicsScene.h"
#include "navigation/Navigation.h"
#include "animation/AnimationCollision.h"
#include <optional>

namespace afterlight {
struct SceneAnimation {
    AssetRef asset;
    std::optional<AssetRef> mesh;
    bool rootMotion = true;
    vec3 rootOffset{0};
    animation::AttributeValues attributes;
};
struct SceneObject {
    std::string id, name;
    vec3 position{0};
    quat rotation{1, 0, 0, 0};
    RenderComponent render;
    std::optional<AssetRef> staticMesh;
    ColliderShape collider;
    BodyMotion motion = BodyMotion::Static;
    uint32_t layer = CollisionLayer::World;
    bool blocking = false, walkable = false, pickable = false;
    bool enabled = true, interactable = false;
    std::optional<SceneAnimation> animation;
    // Authored manual joint poses are retained only for objects without a solver.
    std::vector<PhysicsPose> joints;
    std::vector<AnimationColliderDescription> jointColliders;
};
struct SceneDocument {
    std::vector<AssetRef> scripts; // Map-specific lifecycle code, loaded after project common scripts.
    std::vector<SceneObject> objects;
    std::vector<MaterialDefinition> materials;
    std::map<uint32_t, AssetRef> materialAssets;
    std::vector<Light> lights;
    Camera camera;
    NavigationSettings navigation;
    std::string player;
    std::map<std::string, std::string> references; // Named Map-local Object IDs; resolved after creation.
    Json data = Json::object(); // Explicit persistent gameplay state, never a JS heap dump.
    Json json() const;
    static SceneDocument fromJson(const Json&);
    void validate() const;
};
struct SceneAsset final : Asset {
    SceneDocument scene;
};
} // namespace afterlight
