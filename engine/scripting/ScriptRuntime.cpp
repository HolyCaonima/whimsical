#include "ScriptRuntime.h"
#include "core/CpuProfile.h"
#include "navigation/Navigation.h"
#include "scene/ScenePersistence.h"
#include "UiBindings.h"
#include "uiCore/UiCore.h"
#include <iostream>
#include <stdexcept>
namespace afterlight {
void ScriptRuntime::log(const std::string& text) {
    std::cout << "[JS] " << text << "\n";
    if (logSink_)
        logSink_("[JS] " + text);
}
static AssetManager& assets(duk_context* c) {
    duk_push_heap_stash(c);
    duk_get_prop_string(c, -1, "assets");
    auto* a = static_cast<AssetManager*>(duk_get_pointer(c, -1));
    duk_pop_2(c);
    return *a;
}
static ScriptRuntime& runtime(duk_context* c) {
    duk_push_heap_stash(c);
    duk_get_prop_string(c, -1, "runtime");
    auto* r = static_cast<ScriptRuntime*>(duk_get_pointer(c, -1));
    duk_pop_2(c);
    return *r;
}
static void pushJson(duk_context* c, const Json& j) {
    const auto text = j.dump();
    duk_push_lstring(c, text.data(), text.size());
    duk_json_decode(c, -1);
}

static World& world(duk_context* c) {
    duk_push_heap_stash(c);
    duk_get_prop_string(c, -1, "world");
    auto* w = static_cast<World*>(duk_get_pointer(c, -1));
    duk_pop_2(c);
    return *w;
}
static float num(duk_context* c, int i) {
    return float(duk_require_number(c, i));
}
static void value(duk_context* c, const char* name, double n) {
    duk_push_number(c, n);
    duk_put_prop_string(c, -2, name);
}
static void value(duk_context* c, const char* name, const std::string& text) {
    duk_push_lstring(c, text.data(), text.size());
    duk_put_prop_string(c, -2, name);
}
static void pushVec(duk_context* c, vec3 p) {
    duk_push_object(c);
    value(c, "x", p.x);
    value(c, "y", p.y);
    value(c, "z", p.z);
}
static void pushRotation(duk_context* c, quat q) {
    duk_push_object(c);
    value(c, "x", q.x);
    value(c, "y", q.y);
    value(c, "z", q.z);
    value(c, "w", q.w);
}
static vec3 readVec(duk_context* c, int idx) {
    idx = duk_normalize_index(c, idx);
    vec3 p;
    for (int i = 0; i < 3; i++) {
        duk_get_prop_string(c, idx, i == 0 ? "x" : i == 1 ? "y" : "z");
        p[i] = float(duk_get_number_default(c, -1, 0));
        duk_pop(c);
    }
    return p;
}
static double numberProp(duk_context* c, int idx, const char* name, double fallback) {
    duk_get_prop_string(c, idx, name);
    double result = duk_get_number_default(c, -1, fallback);
    duk_pop(c);
    return result;
}
static bool boolProp(duk_context* c, int idx, const char* name, bool fallback) {
    duk_get_prop_string(c, idx, name);
    bool result = duk_is_undefined(c, -1) ? fallback : duk_get_boolean(c, -1) != 0;
    duk_pop(c);
    return result;
}
static quat readRotation(duk_context* c, int idx) {
    idx = duk_normalize_index(c, idx);
    duk_require_object_coercible(c, idx);
    return quat(float(numberProp(c, idx, "w", 1)), float(numberProp(c, idx, "x", 0)),
                float(numberProp(c, idx, "y", 0)), float(numberProp(c, idx, "z", 0)));
}
static PhysicsPose readPhysicsPose(duk_context* c, int idx) {
    idx = duk_normalize_index(c, idx);
    PhysicsPose p;
    duk_get_prop_string(c, idx, "position");
    if (duk_is_object(c, -1))
        p.position = readVec(c, -1);
    duk_pop(c);
    duk_get_prop_string(c, idx, "rotation");
    if (duk_is_object(c, -1)) {
        p.rotation = readRotation(c, -1);
    }
    duk_pop(c);
    return p;
}
static ColliderShape readShape(duk_context* c, int idx, ColliderShape fallback = {}) {
    idx = duk_normalize_index(c, idx);
    duk_get_prop_string(c, idx, "shape");
    std::string kind = duk_get_string_default(c, -1, "");
    duk_pop(c);
    if (kind.empty())
        return fallback;
    if (kind == "capsule")
        return ColliderShape::capsule(float(numberProp(c, idx, "radius", .4)),
                                      float(numberProp(c, idx, "height", 2)));
    if (kind != "box")
        throw std::invalid_argument("Collider shape must be box or capsule");
    duk_get_prop_string(c, idx, "halfExtents");
    vec3 half = duk_is_object(c, -1) ? readVec(c, -1) : vec3(.5f);
    duk_pop(c);
    return ColliderShape::box(half);
}
static void pushHit(duk_context* c, const PhysicsHit& hit) {
    duk_push_object(c);
    value(c, "owner", hit.owner);
    value(c, "distance", hit.distance);
    value(c, "fraction", hit.fraction);
    value(c, "slot", hit.body.slot);
    value(c, "generation", hit.body.generation);
    pushVec(c, hit.position);
    duk_put_prop_string(c, -2, "position");
    pushVec(c, hit.normal);
    duk_put_prop_string(c, -2, "normal");
}
static QueryFilter readFilter(duk_context* c, int idx) {
    QueryFilter f;
    if (duk_is_undefined(c, idx))
        return f;
    idx = duk_normalize_index(c, idx);
    f.mask = uint32_t(numberProp(c, idx, "mask", CollisionLayer::All));
    f.ignoreOwner = uint32_t(numberProp(c, idx, "ignoreOwner", 0));
    f.blockingOnly = boolProp(c, idx, "blockingOnly", false);
    f.walkableOnly = boolProp(c, idx, "walkableOnly", false);
    f.pickableOnly = boolProp(c, idx, "pickableOnly", false);
    return f;
}
static void pushPose(duk_context* c, const PhysicsPose& p) {
    duk_push_object(c);
    pushVec(c, p.position);
    duk_put_prop_string(c, -2, "position");
    pushRotation(c, p.rotation);
    duk_put_prop_string(c, -2, "rotation");
}
static bool hasComponent(const World& w, Entity e, const std::string& type) {
    return componentCatalog().get(type).present(w.registry(), e);
}
enum Op {
    SceneLoad,
    SceneSave,
    SceneData,
    SetSceneData,
    SceneObject,
    GetObjectPath,
    FindObject,
    CameraState,
    Log,
    MaterialAdd,
    Create,
    AddComponent,
    AddComponents,
    ReadComponent,
    RemoveComponent,
    HasComponent,
    Entities,
    Alive,
    SetParent,
    LocalTransform,
    EntityData,
    SetEntityData,
    AddLight,
    GetPose,
    RenderScale,
    SetTransform,
    MoveBody,
    ShapeSweep,
    ShapeOverlap,
    Move,
    FindPath,
    SetPlayer,
    SetCamera,
    Status,
    Select,
    SetPath,
    SetSolid,
    SetMaterial,
    ReadJson,
    LightIntensity,
    ConfigureCollider,
    VisualPose,
    CharacterHeight,
    RootMotion,
    SetVisible,
    SetEnabled,
    Destroy,
    PhysicsRaycast,
    PhysicsSweep,
    PhysicsOverlap,
    PhysicsRevision,
    PhysicsBodies,
    AnimationJoints,
    AnimationCollider,
    AnimationAttach,
    AnimationInput,
    AnimationAttribute,
    AnimationAttributes,
    AnimationReset,
    AnimationDetach,
    SkinMesh,
    NavigationConfig
};
static duk_ret_t callNative(duk_context* c) {
    auto& w = world(c);
    try {
        switch (duk_get_current_magic(c)) {
        case Log:
            runtime(c).log(duk_safe_to_string(c, 0));
            return 0;
        case MaterialAdd: {
            MaterialDefinition definition;
            definition.shader = assets(c).reference(AssetPath("/Game/shaders/Standard"));
            definition.properties = {{"baseColor", Json::array({num(c, 0), num(c, 1), num(c, 2)})},
                                     {"roughness", num(c, 3)},
                                     {"emission", Json::array({num(c, 4), num(c, 5), num(c, 6)})},
                                     {"metallic", num(c, 7)}};
            w.resources.materials.push_back(definition.resolve(assets(c)));
            duk_push_uint(c, uint32_t(w.resources.materials.size() - 1));
            return 1;
        }
        case Create: {
            duk_dup(c, 0);
            duk_json_encode(c, -1);
            auto description = SceneEntity::fromJson(Json::parse(duk_require_string(c, -1)));
            duk_pop(c);
            duk_push_uint(c, ScenePersistence::createEntity(w, description, assets(c)));
            return 1;
        }
        case AddComponent: {
            Entity e = duk_require_uint(c, 0);
            std::string type = duk_require_string(c, 1);
            duk_dup(c, 2);
            duk_json_encode(c, -1);
            auto data = Json::parse(duk_require_string(c, -1));
            duk_pop(c);
            auto description = SceneEntity::fromJson({{"components", {{type, data}}}});
            ScenePersistence::addComponents(w, e, description, assets(c));
            return 0;
        }
        case AddComponents: {
            auto e = duk_require_uint(c, 0);
            duk_dup(c, 1);
            duk_json_encode(c, -1);
            auto set = ComponentSet::fromJson(Json::parse(duk_require_string(c, -1)));
            duk_pop(c);
            componentCatalog().attach(w, e, set, assets(c));
            return 0;
        }
        case ReadComponent: {
            auto e = duk_require_uint(c, 0);
            w.registry().require(e);
            const auto& type = componentCatalog().get(duk_require_string(c, 1));
            if (!type.present(w.registry(), e))
                throw std::invalid_argument("Component absent: " + type.name);
            auto value = type.capture(w, e, assets(c));
            if (!value)
                throw std::invalid_argument("Component is derived, not authored: " + type.name);
            pushJson(c, type.encode(*value));
            return 1;
        }
        case RemoveComponent: {
            Entity e = duk_require_uint(c, 0);
            std::string type = duk_require_string(c, 1);
            componentCatalog().remove(w, e, type);
            return 0;
        }
        case HasComponent: {
            auto e = duk_require_uint(c, 0);
            w.registry().require(e);
            duk_push_boolean(c, hasComponent(w, e, duk_require_string(c, 1)));
            return 1;
        }
        case Alive:
            duk_push_boolean(c, w.registry().contains(duk_require_uint(c, 0)));
            return 1;
        case Entities: {
            std::vector<const ComponentContract*> types;
            for (duk_uarridx_t i = 0; i < duk_get_length(c, 0); ++i) {
                duk_get_prop_index(c, 0, i);
                types.push_back(&componentCatalog().get(duk_require_string(c, -1)));
                duk_pop(c);
            }
            duk_push_array(c);
            duk_uarridx_t index = 0;
            for (auto e : w.registry().entities()) {
                bool matches = true;
                for (const auto& type : types)
                    matches = type->present(w.registry(), e) && matches;
                if (matches) {
                    duk_push_uint(c, e);
                    duk_put_prop_index(c, -2, index++);
                }
            }
            return 1;
        }
        case SetParent:
            w.transforms.setParent(duk_require_uint(c, 0), duk_require_uint(c, 1),
                                   duk_get_boolean_default(c, 2, true) != 0);
            return 0;
        case LocalTransform:
            w.transforms.setLocal(duk_require_uint(c, 0), readPhysicsPose(c, 1));
            return 0;
        case EntityData:
            pushJson(c, w.get<ScriptData>(duk_require_uint(c, 0)).value);
            return 1;
        case SetEntityData: {
            Entity e = duk_require_uint(c, 0);
            duk_dup(c, 1);
            duk_json_encode(c, -1);
            auto data = Json::parse(duk_require_string(c, -1));
            (void)data.members();
            duk_pop(c);
            w.edit<ScriptData>(e, [&](ScriptData& component) { component.value = std::move(data); });
            return 0;
        }
        case AddLight: {
            Light l;
            l.positionRadius = {num(c, 0), num(c, 1), num(c, 2), num(c, 3)};
            l.colorIntensity = {num(c, 4), num(c, 5), num(c, 6), num(c, 7)};
            w.resources.lights.push_back(l);
            duk_push_uint(c, uint32_t(w.resources.lights.size() - 1));
            return 1;
        }
        case GetPose: {
            const auto& e = w.get<Transform>(duk_require_uint(c, 0));
            pushVec(c, e.world.position);
            value(c, "yaw", e.yaw());
            pushRotation(c, e.world.rotation);
            duk_put_prop_string(c, -2, "rotation");
            return 1;
        }
        case RenderScale:
            w.render.setScale(duk_require_uint(c, 0), readVec(c, 1));
            return 0;
        case SetTransform:
            w.transforms.setTransform(duk_require_uint(c, 0), readPhysicsPose(c, 1));
            return 0;
        case MoveBody: {
            uint32_t id = duk_require_uint(c, 0);
            quat target = duk_is_undefined(c, 2) ? w.get<Transform>(id).world.rotation : readRotation(c, 2);
            auto result =
                w.motion.moveBody(id, readVec(c, 1), target, duk_get_uint_default(c, 3, CollisionLayer::All));
            pushPose(c, result.pose);
            pushVec(c, result.applied);
            duk_put_prop_string(c, -2, "applied");
            duk_push_boolean(c, result.blocked);
            duk_put_prop_string(c, -2, "blocked");
            duk_push_array(c);
            for (duk_uarridx_t i = 0; i < result.contacts.size(); ++i) {
                pushHit(c, result.contacts[i]);
                duk_put_prop_index(c, -2, i);
            }
            duk_put_prop_string(c, -2, "contacts");
            return 1;
        }
        case ShapeSweep: {
            ShapeQuery query{readShape(c, 0), readPhysicsPose(c, 0)};
            quat target = duk_is_undefined(c, 2) ? query.pose.rotation : readRotation(c, 2);
            auto hit = w.physics().sweepShape(query, readVec(c, 1), target, readFilter(c, 3));
            if (hit)
                pushHit(c, *hit);
            else
                duk_push_null(c);
            return 1;
        }
        case ShapeOverlap: {
            auto hits = w.physics().overlapShape({readShape(c, 0), readPhysicsPose(c, 0)}, readFilter(c, 1));
            duk_push_array(c);
            for (duk_uarridx_t i = 0; i < hits.size(); ++i) {
                pushHit(c, hits[i]);
                duk_put_prop_index(c, -2, i);
            }
            return 1;
        }
        case Move: {
            pushVec(c, w.motion.moveCharacter(duk_require_uint(c, 0), vec3(num(c, 1), 0, num(c, 2))));
            return 1;
        }
        case FindPath: {
            auto p = w.motion.findPath(duk_require_uint(c, 0), readVec(c, 1));
            duk_push_array(c);
            for (uint32_t i = 0; i < p.size(); i++) {
                pushVec(c, p[i]);
                duk_put_prop_index(c, -2, i);
            }
            return 1;
        }
        case SetPlayer: {
            auto e = duk_require_uint(c, 0);
            w.registry().require(e);
            w.gameplay.playerId = w.gameplay.selected = e;
            return 0;
        }
        case SetCamera:
            w.resources.camera.target = {num(c, 0), num(c, 1), num(c, 2)};
            w.resources.camera.yaw = num(c, 3);
            w.resources.camera.pitch = num(c, 4);
            w.resources.camera.distance = num(c, 5);
            return 0;
        case Status:
            w.gameplay.state = duk_require_string(c, 0);
            w.gameplay.message = duk_require_string(c, 1);
            return 0;
        case Select: {
            auto e = duk_require_uint(c, 0);
            if (e)
                w.registry().require(e);
            w.gameplay.selected = e;
            return 0;
        }
        case SetPath: {
            w.gameplay.path.clear();
            auto n = duk_get_length(c, 0);
            for (duk_uarridx_t i = 0; i < n; i++) {
                duk_get_prop_index(c, 0, i);
                w.gameplay.path.push_back(readVec(c, -1));
                duk_pop(c);
            }
            w.gameplay.hasDestination = !w.gameplay.path.empty();
            if (w.gameplay.hasDestination)
                w.gameplay.destination = w.gameplay.path.back();
            return 0;
        }
        case SetSolid:
            w.motion.setSolid(duk_require_uint(c, 0), duk_get_boolean(c, 1) != 0);
            return 0;
        case SetMaterial: {
            auto i = duk_require_uint(c, 1);
            if (i >= w.resources.materials.size())
                throw std::runtime_error("Invalid material");
            w.render.setMaterial(duk_require_uint(c, 0), i);
            return 0;
        }
        case ReadJson: {
            auto asset = assets(c).load<DataAsset>(AssetPath(duk_require_string(c, 0)));
            pushJson(c, asset->data);
            return 1;
        }
        case SceneLoad:
            runtime(c).requestScene(AssetPath(duk_require_string(c, 0)).string());
            return 0;
        case SceneSave: {
            auto ref = runtime(c).saveScene(AssetPath(duk_require_string(c, 0)), duk_require_string(c, 1));
            pushJson(c, ref.json());
            return 1;
        }
        case SceneData:
            pushJson(c, w.resources.data);
            return 1;
        case SetSceneData: {
            duk_dup(c, 0);
            duk_json_encode(c, -1);
            auto data = Json::parse(duk_require_string(c, -1));
            (void)data.members();
            w.resources.data = std::move(data);
            duk_pop(c);
            return 0;
        }
        case SceneObject:
            duk_push_uint(c, w.findObject(w.resources.references.at(duk_require_string(c, 0))));
            return 1;
        case GetObjectPath: {
            auto path = w.objectPath(duk_require_uint(c, 0)).string();
            duk_push_string(c, path.c_str());
            return 1;
        }
        case FindObject:
            duk_push_uint(c, w.resolveObject(ObjectPath(duk_require_string(c, 0))));
            return 1;
        case CameraState:
            pushJson(c, {{"yaw", w.resources.camera.yaw},
                         {"pitch", w.resources.camera.pitch},
                         {"distance", w.resources.camera.distance},
                         {"x", w.resources.camera.target.x},
                         {"z", w.resources.camera.target.z}});
            return 1;
        case LightIntensity:
            w.resources.lights.at(duk_require_uint(c, 0)).colorIntensity.w = num(c, 1);
            return 0;
        case ConfigureCollider: {
            uint32_t id = duk_require_uint(c, 0);
            const auto b = w.physics().body(w.get<Collider>(id).body);
            w.motion.setColliderShape(id, readShape(c, 1, b.shape));
            w.motion.configureCollider(
                id, uint32_t(numberProp(c, 1, "layer", b.layer)), boolProp(c, 1, "blocking", b.blocking),
                boolProp(c, 1, "walkable", b.walkable), boolProp(c, 1, "pickable", b.pickable));
            return 0;
        }
        case VisualPose:
            w.render.setVisualPose(duk_require_uint(c, 0), readVec(c, 1), readVec(c, 2));
            return 0;
        case CharacterHeight:
            duk_push_number(c, w.motion.setCharacterHeight(duk_require_uint(c, 0), num(c, 1)));
            return 1;
        case RootMotion:
            pushVec(c, w.motion.rootMotion(duk_require_uint(c, 0), readVec(c, 1), num(c, 2)));
            return 1;
        case SetVisible:
            w.render.setVisible(duk_require_uint(c, 0), duk_get_boolean(c, 1) != 0);
            return 0;
        case SetEnabled:
            w.setEnabled(duk_require_uint(c, 0), duk_get_boolean(c, 1) != 0);
            return 0;
        case Destroy:
            w.destroy(duk_require_uint(c, 0));
            return 0;
        case PhysicsBodies: {
            const auto& physics = w.physics();
            duk_push_array(c);
            duk_uarridx_t index = 0;
            for (auto handle : physics.bodies(readFilter(c, 0))) {
                const auto& body = physics.body(handle);
                auto bounds = physics.bounds(handle);
                duk_push_object(c);
                value(c, "owner", body.owner);
                value(c, "slot", handle.slot);
                value(c, "generation", handle.generation);
                value(c, "layer", body.layer);
                pushVec(c, bounds.min);
                duk_put_prop_string(c, -2, "min");
                pushVec(c, bounds.max);
                duk_put_prop_string(c, -2, "max");
                for (auto property : {std::pair<const char*, bool>{"blocking", body.blocking},
                                      {"walkable", body.walkable},
                                      {"pickable", body.pickable}}) {
                    duk_push_boolean(c, property.second);
                    duk_put_prop_string(c, -2, property.first);
                }
                duk_put_prop_index(c, -2, index++);
            }
            return 1;
        }
        case PhysicsRevision:
            duk_push_number(c, double(w.physics().revision()));
            return 1;
        case PhysicsRaycast: {
            QueryFilter filter;
            filter.mask = duk_get_uint_default(c, 3, CollisionLayer::All);
            auto hit = w.physics().raycast(readVec(c, 0), readVec(c, 1), num(c, 2), filter);
            if (hit)
                pushHit(c, *hit);
            else
                duk_push_null(c);
            return 1;
        }
        case PhysicsOverlap: {
            QueryFilter filter;
            filter.mask = duk_get_uint_default(c, 3, CollisionLayer::All);
            filter.ignoreOwner = duk_get_uint_default(c, 4, 0);
            auto hits = w.physics().overlapCapsule({readVec(c, 0), num(c, 1), num(c, 2)}, filter);
            duk_push_array(c);
            for (uint32_t i = 0; i < hits.size(); ++i) {
                pushHit(c, hits[i]);
                duk_put_prop_index(c, -2, i);
            }
            return 1;
        }
        case PhysicsSweep: {
            QueryFilter filter;
            filter.blockingOnly = true;
            filter.mask = duk_get_uint_default(c, 4, CollisionLayer::World | CollisionLayer::Character);
            filter.ignoreOwner = duk_get_uint_default(c, 5, 0);
            auto hit = w.physics().sweepCapsule({readVec(c, 0), num(c, 1), num(c, 2)}, readVec(c, 3), filter);
            if (hit)
                pushHit(c, *hit);
            else
                duk_push_null(c);
            return 1;
        }
        case AnimationJoints: {
            std::vector<PhysicsPose> joints;
            for (duk_uarridx_t i = 0; i < duk_get_length(c, 1); ++i) {
                duk_get_prop_index(c, 1, i);
                joints.push_back(readPhysicsPose(c, -1));
                duk_pop(c);
            }
            w.animation.setAnimationJoints(duk_require_uint(c, 0), std::move(joints));
            return 0;
        }
        case AnimationCollider: {
            auto h = w.animation.addAnimationCollider(duk_require_uint(c, 0), duk_require_uint(c, 1),
                                                      readShape(c, 2), readPhysicsPose(c, 2),
                                                      boolProp(c, 2, "blocking", false));
            duk_push_object(c);
            value(c, "slot", h.slot);
            value(c, "generation", h.generation);
            return 1;
        }
        case AnimationAttach: {
            SceneAnimation animation;
            animation.asset = assets(c).reference(AssetPath(duk_require_string(c, 1)));
            if (duk_is_object(c, 2)) {
                if (duk_has_prop_string(c, 2, "rootMotion"))
                    throw std::invalid_argument("Use the rootMotion component");
                duk_get_prop_string(c, 2, "rootOffset");
                if (duk_is_object(c, -1))
                    animation.rootOffset = readVec(c, -1);
                duk_pop(c);
            }
            ComponentSet set;
            set.set(std::move(animation));
            componentCatalog().attach(w, duk_require_uint(c, 0), set, assets(c));
            return 0;
        }
        case AnimationAttribute:
            w.animation.setAnimationAttribute(duk_require_uint(c, 0), duk_require_string(c, 1),
                                              duk_require_string(c, 2));
            return 0;
        case AnimationAttributes: {
            auto inspection = w.animation.inspectAnimation(duk_require_uint(c, 0));
            duk_push_object(c);
            value(c, "solver", inspection.solver);
            value(c, "name", inspection.name);
            duk_push_array(c);
            for (duk_uarridx_t i = 0; i < inspection.schema.size(); ++i) {
                const auto& attribute = inspection.schema[i];
                duk_push_object(c);
                value(c, "key", attribute.key);
                value(c, "label", attribute.label);
                value(c, "type", std::string("enum"));
                value(c, "value", inspection.values.at(attribute.key));
                value(c, "defaultValue", attribute.defaultValue);
                duk_push_array(c);
                for (duk_uarridx_t k = 0; k < attribute.options.size(); ++k) {
                    duk_push_object(c);
                    value(c, "value", attribute.options[k].value);
                    value(c, "label", attribute.options[k].label);
                    duk_put_prop_index(c, -2, k);
                }
                duk_put_prop_string(c, -2, "options");
                duk_put_prop_index(c, -2, i);
            }
            duk_put_prop_string(c, -2, "attributes");
            return 1;
        }
        case AnimationInput: {
            animation::Input input;
            duk_get_prop_string(c, 1, "velocity");
            if (duk_is_object(c, -1))
                input.desiredVelocity = readVec(c, -1);
            duk_pop(c);
            duk_get_prop_string(c, 1, "facing");
            if (duk_is_object(c, -1))
                input.facing = readVec(c, -1);
            duk_pop(c);
            duk_get_prop_string(c, 1, "action");
            input.action = duk_get_string_default(c, -1, "Idle");
            duk_pop(c);
            input.speedScale = float(numberProp(c, 1, "speedScale", 1));
            duk_get_prop_string(c, 1, "trajectory");
            for (duk_uarridx_t i = 0; i < duk_get_length(c, -1); ++i) {
                duk_get_prop_index(c, -1, i);
                int p = duk_normalize_index(c, -1);
                auto pose = readPhysicsPose(c, p);
                animation::TrajectoryPoint point;
                point.time = float(numberProp(c, p, "time", 0));
                point.transform = {pose.position, pose.rotation};
                duk_get_prop_string(c, p, "velocity");
                if (duk_is_object(c, -1))
                    point.velocity = readVec(c, -1);
                duk_pop(c);
                input.trajectory.push_back(point);
                duk_pop(c);
            }
            duk_pop(c);
            w.animation.setAnimationInput(duk_require_uint(c, 0), std::move(input));
            return 0;
        }
        case AnimationReset:
            w.animation.resetAnimation(duk_require_uint(c, 0));
            return 0;
        case AnimationDetach:
            w.animation.detachAnimation(duk_require_uint(c, 0));
            return 0;
        case SkinMesh: {
            w.animation.setSkinnedMesh(duk_require_uint(c, 0),
                                       assets(c).load<SkinnedMesh>(AssetPath(duk_require_string(c, 1))));
            return 0;
        }
        case NavigationConfig: {
            duk_get_prop_string(c, 0, "min");
            auto min = readVec(c, -1);
            duk_pop(c);
            duk_get_prop_string(c, 0, "max");
            auto max = readVec(c, -1);
            duk_pop(c);
            float cell = float(numberProp(c, 0, "cellSize", .25));
            if (!std::isfinite(cell) || cell < .05f || glm::any(glm::lessThanEqual(max, min)))
                throw std::invalid_argument("Invalid navigation configuration");
            w.resources.navigation.min = min;
            w.resources.navigation.max = max;
            w.resources.navigation.cellSize = cell;
            return 0;
        }
        }
        return 0;
    } catch (const std::exception& e) {
        duk_push_error_object(c, DUK_ERR_ERROR, "%s", e.what());
    }
    duk_throw_raw(c);
}
ScriptRuntime::ScriptRuntime(World& w, AssetManager& a, ui::UiCore* ui)
    : world_(w), assets_(a), owner_(std::this_thread::get_id()), ui_(ui) {
    createContext();
}
void ScriptRuntime::createContext() {
    context_ = duk_create_heap_default();
    if (!context_)
        throw std::runtime_error("JS heap creation failed");
    duk_push_heap_stash(context_);
    duk_push_pointer(context_, &world_);
    duk_put_prop_string(context_, -2, "world");
    duk_push_pointer(context_, &assets_);
    duk_put_prop_string(context_, -2, "assets");
    duk_push_pointer(context_, this);
    duk_put_prop_string(context_, -2, "runtime");
    duk_pop(context_);
    duk_push_object(context_);
    struct Binding {
        const char* name;
        Op op;
        int nargs;
    };
    const Binding bindings[] = {{"loadScene", SceneLoad, 1},
                                {"saveScene", SceneSave, 2},
                                {"sceneData", SceneData, 0},
                                {"setSceneData", SetSceneData, 1},
                                {"sceneObject", SceneObject, 1},
                                {"objectPath", GetObjectPath, 1},
                                {"findObject", FindObject, 1},
                                {"cameraState", CameraState, 0},
                                {"log", Log, 1},
                                {"material", MaterialAdd, 8},
                                {"create", Create, 1},
                                {"addComponent", AddComponent, 3},
                                {"addComponents", AddComponents, 2},
                                {"component", ReadComponent, 2},
                                {"removeComponent", RemoveComponent, 2},
                                {"hasComponent", HasComponent, 2},
                                {"entities", Entities, 1},
                                {"alive", Alive, 1},
                                {"parent", SetParent, 3},
                                {"localTransform", LocalTransform, 2},
                                {"data", EntityData, 1},
                                {"setData", SetEntityData, 2},
                                {"light", AddLight, 8},
                                {"position", GetPose, 1},
                                {"renderScale", RenderScale, 2},
                                {"transform", SetTransform, 2},
                                {"moveBody", MoveBody, 4},
                                {"physicsShapeSweep", ShapeSweep, 4},
                                {"physicsShapeOverlap", ShapeOverlap, 2},
                                {"move", Move, 3},
                                {"findPath", FindPath, 2},
                                {"setPlayer", SetPlayer, 1},
                                {"camera", SetCamera, 6},
                                {"status", Status, 2},
                                {"select", Select, 1},
                                {"showPath", SetPath, 1},
                                {"solid", SetSolid, 2},
                                {"setMaterial", SetMaterial, 2},
                                {"readJson", ReadJson, 1},
                                {"lightIntensity", LightIntensity, 2},
                                {"collider", ConfigureCollider, 2},
                                {"visualPose", VisualPose, 3},
                                {"characterHeight", CharacterHeight, 2},
                                {"rootMotion", RootMotion, 3},
                                {"visible", SetVisible, 2},
                                {"enabled", SetEnabled, 2},
                                {"destroy", Destroy, 1},
                                {"physicsRaycast", PhysicsRaycast, 4},
                                {"physicsSweep", PhysicsSweep, 6},
                                {"physicsOverlap", PhysicsOverlap, 5},
                                {"physicsRevision", PhysicsRevision, 0},
                                {"physicsBodies", PhysicsBodies, 1},
                                {"animationJoints", AnimationJoints, 2},
                                {"animationCollider", AnimationCollider, 3},
                                {"animation", AnimationAttach, 3},
                                {"animationInput", AnimationInput, 2},
                                {"animationAttribute", AnimationAttribute, 3},
                                {"animationAttributes", AnimationAttributes, 1},
                                {"animationReset", AnimationReset, 1},
                                {"animationDetach", AnimationDetach, 1},
                                {"skinMesh", SkinMesh, 2},
                                {"navigation", NavigationConfig, 1}};
    for (auto& b : bindings) {
        duk_push_c_function(context_, callNative, b.nargs);
        duk_set_magic(context_, -1, b.op);
        duk_put_prop_string(context_, -2, b.name);
    }
    duk_put_global_string(context_, "Engine");
    if (ui_) {
        uiBindings_ = std::make_unique<UiBindings>(context_, *ui_, [this](const auto& text) { log(text); });
        uiBindings_->setVisible(hudEnabled_);
    }
}
ScriptRuntime::~ScriptRuntime() {
    uiBindings_.reset();
    if (context_)
        duk_destroy_heap(context_);
}
void ScriptRuntime::checkedCall(int args) {
    if (duk_pcall(context_, args) != 0) {
        std::string e = duk_safe_to_stacktrace(context_, -1);
        duk_pop(context_);
        throw std::runtime_error("JavaScript: " + e);
    }
    duk_pop(context_);
}
void ScriptRuntime::evaluateFile(const std::string& path) {
    auto asset = assets_.load<ScriptAsset>(AssetPath(path));
    evaluateSource(asset->source, path);
}
void ScriptRuntime::execute(const std::string& source, const std::string& label) {
    evaluateSource(source, label);
    processSceneRequest();
}
void ScriptRuntime::evaluateSource(const std::string& source, const std::string& label) {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("JS execution must run on the owner thread");
    duk_push_string(context_, label.c_str());
    if (duk_pcompile_lstring_filename(context_, DUK_COMPILE_EVAL, source.data(), source.size()) != 0) {
        std::string error = duk_safe_to_string(context_, -1);
        duk_pop(context_);
        throw std::runtime_error(error);
    }
    checkedCall(0);
}
void ScriptRuntime::initialize() {
    if (!assets_.project().startupMap().empty())
        loadScene(assets_.project().startupMap());
    else
        startScripts();
}
void ScriptRuntime::startScripts() {
    for (const auto& path : assets_.project().scripts())
        evaluateFile(path.string());
    for (const auto& script : world_.resources.scripts)
        evaluateFile(assets_.resolve(script).path.string());
    duk_get_global_string(context_, "initialize");
    if (duk_is_function(context_, -1))
        checkedCall(0);
    else
        duk_pop(context_);
    updateUi(0);
}
void ScriptRuntime::updateUi(float dt) {
    if (!ui_)
        return;
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("UI updates require the owner thread");
    duk_get_global_string(context_, "updateUI");
    if (duk_is_function(context_, -1)) {
        duk_push_number(context_, dt);
        checkedCall(1);
    } else
        duk_pop(context_);
    processSceneRequest();
}
void ScriptRuntime::setHudEnabled(bool enabled) {
    hudEnabled_ = enabled;
    if (uiBindings_)
        uiBindings_->setVisible(enabled);
}
void ScriptRuntime::processUiInput(Input& input) {
    if (ui_)
        ui_->processInput(input);
    processSceneRequest();
}
void ScriptRuntime::loadScene(const AssetPath& path) {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("Scene loading requires the owner thread");
    ScenePersistence::load(world_, assets_, path);
    uiBindings_.reset();
    duk_destroy_heap(context_);
    context_ = nullptr;
    createContext();
    startScripts();
}
AssetRef ScriptRuntime::saveScene(const AssetPath& path, const std::string& name) {
    return ScenePersistence::save(world_, assets_, path, name);
}
void ScriptRuntime::processSceneRequest() {
    if (pendingScene_.empty())
        return;
    auto path = std::move(pendingScene_);
    pendingScene_.clear();
    loadScene(AssetPath(path));
}
void ScriptRuntime::tick(float dt, const Input& rawInput) {
    CpuScope inputScope("Input / Picking / JS Arguments");
    if (std::this_thread::get_id() != owner_)
        throw std::runtime_error("JS accessed outside engine thread");
    Input input = rawInput;
    bool captured = input.pointerCaptured;
    world_.gameplay.hovered = captured ? 0 : world_.pick(input.mouseX, input.mouseY, input);
    auto ground = world_.groundAt(input.mouseX, input.mouseY, input);
    duk_get_global_string(context_, "fixedUpdate");
    if (duk_is_undefined(context_, -1)) {
        duk_pop(context_);
        inputScope.finish();
        world_.update(dt);
        processSceneRequest();
        updateUi(dt);
        return;
    }
    duk_push_number(context_, dt);
    duk_push_object(context_);
    duk_push_boolean(context_, captured);
    duk_put_prop_string(context_, -2, "pointerCaptured");
    duk_push_boolean(context_, input.keyboardCaptured);
    duk_put_prop_string(context_, -2, "keyboardCaptured");
    for (auto pair : {std::pair<const char*, const std::array<bool, 256>*>{"keys", &input.keys},
                      {"pressed", &input.pressed}}) {
        duk_push_array(context_);
        for (uint32_t i = 0; i < 256; i++) {
            duk_push_boolean(context_, (*pair.second)[i]);
            duk_put_prop_index(context_, -2, i);
        }
        duk_put_prop_string(context_, -2, pair.first);
    }
    value(context_, "x", input.mouseX);
    value(context_, "y", input.mouseY);
    value(context_, "dx", input.deltaX);
    value(context_, "dy", input.deltaY);
    value(context_, "wheel", input.wheel);
    value(context_, "width", input.width);
    value(context_, "height", input.height);
    value(context_, "groundX", ground ? ground->x : 0);
    value(context_, "groundY", ground ? ground->y : 0);
    value(context_, "groundZ", ground ? ground->z : 0);
    duk_push_boolean(context_, ground.has_value());
    duk_put_prop_string(context_, -2, "groundValid");
    value(context_, "picked", world_.gameplay.hovered);
    for (auto b : {std::pair<const char*, bool>{"leftPressed", input.leftPressed},
                   {"rightPressed", input.rightPressed},
                   {"middle", input.middle},
                   {"focused", input.focused}}) {
        duk_push_boolean(context_, b.second);
        duk_put_prop_string(context_, -2, b.first);
    }
    inputScope.finish();
    {
        CpuScope scope("JS fixedUpdate");
        checkedCall(2);
    }
    world_.update(dt);
    processSceneRequest();
    updateUi(dt);
}
} // namespace afterlight
