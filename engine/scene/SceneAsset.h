#pragma once
#include "assets/Asset.h"
#include "core/Types.h"
#include "physics/PhysicsScene.h"
#include "navigation/Navigation.h"
#include "animation/AnimationCollision.h"
#include <optional>
namespace afterlight {
struct SceneTransform {
    PhysicsPose local;
    std::string parent;
};
struct SceneRender {
    RenderComponent appearance;
    std::optional<AssetRef> mesh;
};
struct SceneCollider {
    ColliderShape shape;
    BodyMotion motion = BodyMotion::Static;
    uint32_t layer = CollisionLayer::World;
    bool blocking = false, walkable = false, pickable = false;
};
struct SceneAnimation {
    AssetRef asset;
    bool rootMotion = true;
    vec3 rootOffset{0};
    animation::AttributeValues attributes;
};
struct SceneEntity {
    std::string id, name;
    bool enabled = true, interactable = false;
    std::optional<SceneTransform> transform;
    std::optional<SceneRender> render;
    std::optional<SceneCollider> collider;
    std::optional<SceneAnimation> animation;
    std::optional<AssetRef> skin;
    std::optional<Json> data;
    std::optional<std::vector<PhysicsPose>> joints;
    std::vector<AnimationColliderDescription> jointColliders;
    Json json() const;
    static SceneEntity fromJson(const Json&);
};
struct SceneDocument {
    std::vector<AssetRef> scripts;
    std::vector<SceneEntity> entities;
    std::vector<MaterialDefinition> materials;
    std::map<uint32_t, AssetRef> materialAssets;
    std::vector<Light> lights;
    Camera camera;
    NavigationSettings navigation;
    std::string player;
    std::map<std::string, std::string> references;
    Json data = Json::object();
    Json json() const;
    static SceneDocument fromJson(const Json&);
    void validate() const;
};
struct SceneAsset final : Asset {
    SceneDocument scene;
};
} // namespace afterlight
