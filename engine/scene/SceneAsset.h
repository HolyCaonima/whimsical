#pragma once
#include "core/SpatialTransform.h"
#include "assets/Asset.h"
#include "core/Types.h"
#include "physics/PhysicsScene.h"
#include "navigation/Navigation.h"
#include "animation/AnimationCollision.h"
#include <optional>
#include "ecs/ComponentCatalog.h"
namespace whimsical {
struct SceneTransform {
    TransformPose local;
    std::string parent;
};
struct SceneRender {
    RenderComponent appearance;
    std::optional<AssetRef> mesh;
    AssetRef material;
};
struct SceneDrawEntityID {
    AssetRef target;
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
// Resource descriptions can be edited without serializing or reinstantiating entities.
// Maps and live resource access use this same codec and validation contract.
struct SceneResourceDescription {
    std::vector<AssetRef> scripts;
    Camera camera;
    NavigationSettings navigation;
    std::map<std::string, std::string> references;
    Json data = Json::object();
    Json json() const;
    static SceneResourceDescription fromJson(const Json&);
    void validate() const;
};
struct SceneDocument : SceneResourceDescription {
    std::vector<SceneEntity> entities;
    Json json() const;
    static SceneDocument fromJson(const Json&);
    void validate() const;
};
struct SceneAsset final : Asset {
    SceneDocument scene;
};
} // namespace whimsical
