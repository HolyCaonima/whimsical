#pragma once
#include "assets/Asset.h"
#include "core/Types.h"
#include "physics/PhysicsScene.h"
#include "navigation/Navigation.h"
#include "animation/AnimationCollision.h"
#include <optional>
#include "ecs/ComponentCatalog.h"
namespace afterlight {
struct SceneTransform {
    PhysicsPose local;
    std::string parent;
};
struct SceneRender {
    RenderComponent appearance;
    std::optional<AssetRef> mesh;
};
using SceneCollider = ColliderSettings;
struct SceneAnimation {
    AssetRef asset;
    vec3 rootOffset{0};
    animation::AttributeValues attributes;
};
struct SceneJoints {
    std::vector<PhysicsPose> poses;
    std::vector<animation::Joint> layout;
};
struct SceneJointColliders {
    std::vector<AnimationColliderDescription> bindings;
};
struct SceneEntity {
    std::string id, name;
    bool enabled = true;
    ComponentSet components;
    Json json() const;
    static SceneEntity fromJson(const Json&);
};
struct SceneDocument {
    std::vector<AssetRef> scripts;
    std::vector<SceneEntity> entities;
    std::vector<MaterialDefinition> materials;
    std::map<uint32_t, AssetRef> materialAssets;
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
