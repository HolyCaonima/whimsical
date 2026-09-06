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
static PhysicsPose readPhysicsPose(duk_context* c, int idx) {
    idx = duk_normalize_index(c, idx);
    PhysicsPose p;
    duk_get_prop_string(c, idx, "position");
    if (duk_is_object(c, -1))
        p.position = readVec(c, -1);
    duk_pop(c);
    duk_get_prop_string(c, idx, "rotation");
    if (duk_is_object(c, -1)) {
        int q = duk_normalize_index(c, -1);
        p.rotation = quat(float(numberProp(c, q, "w", 1)), float(numberProp(c, q, "x", 0)),
                          float(numberProp(c, q, "y", 0)), float(numberProp(c, q, "z", 0)));
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
    Spawn,
    AddLight,
    GetPose,
    SetPose,
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
            Material m;
            m.albedoRoughness = {num(c, 0), num(c, 1), num(c, 2), num(c, 3)};
            m.emissionMetallic = {num(c, 4), num(c, 5), num(c, 6), num(c, 7)};
            w.materials.push_back(m);
            duk_push_uint(c, uint32_t(w.materials.size() - 1));
            return 1;
        }
        case Spawn: {
            auto id =
                w.spawn(duk_require_string(c, 0), duk_get_boolean(c, 1) ? Shape::Capsule : Shape::Box,
                        {num(c, 2), num(c, 3), num(c, 4)}, {num(c, 5), num(c, 6), num(c, 7)},
                        duk_require_uint(c, 8), duk_get_boolean(c, 9) != 0, duk_get_boolean(c, 10) != 0);
            duk_push_uint(c, id);
            return 1;
        }
        case AddLight: {
            Light l;
            l.positionRadius = {num(c, 0), num(c, 1), num(c, 2), num(c, 3)};
            l.colorIntensity = {num(c, 4), num(c, 5), num(c, 6), num(c, 7)};
            w.lights.push_back(l);
            duk_push_uint(c, uint32_t(w.lights.size() - 1));
            return 1;
        }
        case GetPose: {
            const auto& e = w.entity(duk_require_uint(c, 0));
            pushVec(c, e.position);
            value(c, "yaw", e.yaw);
            return 1;
        }
        case SetPose: {
            w.setPose(duk_require_uint(c, 0), {num(c, 1), num(c, 2), num(c, 3)}, num(c, 4), num(c, 5));
            return 0;
        }
        case Move: {
            pushVec(c, w.moveCharacter(duk_require_uint(c, 0), vec3(num(c, 1), 0, num(c, 2))));
            return 1;
        }
        case FindPath: {
            auto p = w.findPath(duk_require_uint(c, 0), readVec(c, 1));
            duk_push_array(c);
            for (uint32_t i = 0; i < p.size(); i++) {
                pushVec(c, p[i]);
                duk_put_prop_index(c, -2, i);
            }
            return 1;
        }
        case SetPlayer:
            w.playerId = duk_require_uint(c, 0);
            w.entity(w.playerId);
            w.selected = w.playerId;
            return 0;
        case SetCamera:
            w.camera.target = {num(c, 0), num(c, 1), num(c, 2)};
            w.camera.yaw = num(c, 3);
            w.camera.pitch = num(c, 4);
            w.camera.distance = num(c, 5);
            return 0;
        case Status:
            w.state = duk_require_string(c, 0);
            w.message = duk_require_string(c, 1);
            return 0;
        case Select:
            w.selected = duk_require_uint(c, 0);
            if (w.selected)
                w.entity(w.selected);
            return 0;
        case SetPath: {
            w.path.clear();
            auto n = duk_get_length(c, 0);
            for (duk_uarridx_t i = 0; i < n; i++) {
                duk_get_prop_index(c, 0, i);
                w.path.push_back(readVec(c, -1));
                duk_pop(c);
            }
            w.hasDestination = !w.path.empty();
            if (w.hasDestination)
                w.destination = w.path.back();
            return 0;
        }
        case SetSolid:
            w.setSolid(duk_require_uint(c, 0), duk_get_boolean(c, 1) != 0);
            return 0;
        case SetMaterial: {
            auto i = duk_require_uint(c, 1);
            if (i >= w.materials.size())
                throw std::runtime_error("Invalid material");
            w.setMaterial(duk_require_uint(c, 0), i);
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
            pushJson(c, w.sceneData);
            return 1;
        case SetSceneData: {
            duk_dup(c, 0);
            duk_json_encode(c, -1);
            auto data = Json::parse(duk_require_string(c, -1));
            (void)data.members();
            w.sceneData = std::move(data);
            duk_pop(c);
            return 0;
        }
        case SceneObject:
            duk_push_uint(c, w.findObject(w.sceneReferences.at(duk_require_string(c, 0))));
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
            pushJson(c, {{"yaw", w.camera.yaw},
                         {"pitch", w.camera.pitch},
                         {"distance", w.camera.distance},
                         {"x", w.camera.target.x},
                         {"z", w.camera.target.z}});
            return 1;
        case LightIntensity:
            w.lights.at(duk_require_uint(c, 0)).colorIntensity.w = num(c, 1);
            return 0;
        case ConfigureCollider: {
            uint32_t id = duk_require_uint(c, 0);
            const auto b = w.physics().body(w.entity(id).physical);
            w.setColliderShape(id, readShape(c, 1, b.shape));
            w.configureCollider(
                id, uint32_t(numberProp(c, 1, "layer", b.layer)), boolProp(c, 1, "blocking", b.blocking),
                boolProp(c, 1, "walkable", b.walkable), boolProp(c, 1, "pickable", b.pickable));
            return 0;
        }
        case VisualPose:
            w.setVisualPose(duk_require_uint(c, 0), readVec(c, 1), readVec(c, 2));
            return 0;
        case CharacterHeight:
            duk_push_number(c, w.setCharacterHeight(duk_require_uint(c, 0), num(c, 1)));
            return 1;
        case RootMotion:
            pushVec(c, w.rootMotion(duk_require_uint(c, 0), readVec(c, 1), num(c, 2)));
            return 1;
        case SetVisible:
            w.setVisible(duk_require_uint(c, 0), duk_get_boolean(c, 1) != 0);
            return 0;
        case SetEnabled:
            w.setEnabled(duk_require_uint(c, 0), duk_get_boolean(c, 1) != 0);
            return 0;
        case Destroy:
            w.destroy(duk_require_uint(c, 0));
            return 0;
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
            w.setAnimationJoints(duk_require_uint(c, 0), std::move(joints));
            return 0;
        }
        case AnimationCollider: {
            auto h = w.addAnimationCollider(duk_require_uint(c, 0), duk_require_uint(c, 1), readShape(c, 2),
                                            readPhysicsPose(c, 2), boolProp(c, 2, "blocking", false));
            duk_push_object(c);
            value(c, "slot", h.slot);
            value(c, "generation", h.generation);
            return 1;
        }
        case AnimationAttach: {
            auto asset = assets(c).load<animation::Asset>(AssetPath(duk_require_string(c, 1)));
            bool applyRoot = true;
            vec3 offset(0);
            if (duk_is_object(c, 2)) {
                applyRoot = boolProp(c, 2, "rootMotion", true);
                duk_get_prop_string(c, 2, "rootOffset");
                if (duk_is_object(c, -1))
                    offset = readVec(c, -1);
                duk_pop(c);
            }
            w.attachAnimation(duk_require_uint(c, 0), *asset, applyRoot, offset);
            return 0;
        }
        case AnimationAttribute:
            w.setAnimationAttribute(duk_require_uint(c, 0), duk_require_string(c, 1),
                                    duk_require_string(c, 2));
            return 0;
        case AnimationAttributes: {
            auto inspection = w.inspectAnimation(duk_require_uint(c, 0));
            duk_push_object(c);
            value(c, "solver", inspection.solver);
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
            w.setAnimationInput(duk_require_uint(c, 0), std::move(input));
            return 0;
        }
        case AnimationReset:
            w.resetAnimation(duk_require_uint(c, 0));
            return 0;
        case AnimationDetach:
            w.detachAnimation(duk_require_uint(c, 0));
            return 0;
        case SkinMesh: {
            w.setSkinnedMesh(duk_require_uint(c, 0),
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
            w.navigation.min = min;
            w.navigation.max = max;
            w.navigation.cellSize = cell;
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
                                {"spawn", Spawn, 11},
                                {"light", AddLight, 8},
                                {"position", GetPose, 1},
                                {"pose", SetPose, 6},
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
    for (const auto& script : world_.sceneScripts)
        evaluateFile(assets_.resolve(script).path.string());
    duk_get_global_string(context_, "initialize");
    if (duk_is_function(context_, -1))
        checkedCall(0);
    else
        duk_pop(context_);
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
    world_.hovered = captured ? 0 : world_.pick(input.mouseX, input.mouseY, input);
    auto ground = world_.groundAt(input.mouseX, input.mouseY, input);
    duk_get_global_string(context_, "fixedUpdate");
    if (duk_is_undefined(context_, -1)) {
        duk_pop(context_);
        inputScope.finish();
        world_.updateAnimations(dt);
        processSceneRequest();
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
    value(context_, "picked", world_.hovered);
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
    world_.updateAnimations(dt);
    processSceneRequest();
}
} // namespace afterlight
