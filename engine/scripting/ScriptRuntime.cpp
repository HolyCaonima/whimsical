#include "ScriptRuntime.h"
#include "RuntimeHost.h"
#include "RuntimeBindings.h"
#include "core/CpuProfile.h"
#include "navigation/Navigation.h"
#include "scene/ScenePersistence.h"
#include "UiBindings.h"
#include "physics/dynamics/adapter/ScriptBindings.h"
#include "uiCore/UiCore.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
namespace whimsical {
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

static Json argumentJson(duk_context* c, int index) {
    duk_dup(c, index);
    duk_json_encode(c, -1);
    auto result = Json::parse(duk_require_string(c, -1));
    duk_pop(c);
    return result;
}
static Json contentSourceJson(const ContentSourceRef& source) {
    return {{"mount", source->mount}, {"source", source->id},
            {"root", source->root.u8string()}, {"writable", source->writable}, {"shared", source->shared}};
}
// Keep RAII owners out of the callback frame that returns errors through Duktape longjmp.
__declspec(noinline) static duk_ret_t contentDispatch(duk_context* c) {
    auto& manager = assets(c);
    AssetManager::Scope caller(manager, {}, false); // Content management uses explicit mounted paths.
    const int op = duk_get_current_magic(c);
    if (op == 0) {
        auto source =
            manager.mount(duk_require_string(c, 0), std::filesystem::u8path(duk_require_string(c, 1)),
                          duk_get_boolean_default(c, 2, true) != 0);
        pushJson(c, contentSourceJson(source));
    } else if (op == 1) {
        manager.unmount(duk_require_string(c, 0));
        return 0;
    } else if (op == 2) {
        Json list = Json::array();
        for (auto source : manager.mounts()->sources())
            list.push(contentSourceJson(source));
        pushJson(c, list);
    } else if (op == 3) {
        Json list = Json::array();
        for (const auto& ref : manager.browse(duk_require_string(c, 0)))
            list.push(ref.json());
        pushJson(c, list);
    } else if (op == 4 || op == 5 || op == 6 || op == 9) {
        bool path = duk_is_string(c, 0);
        AssetRef ref;
        AssetPath target;
        if (path)
            target = AssetPath(duk_require_string(c, 0));
        else
            ref = AssetRef::fromJson(argumentJson(c, 0));
        if (op == 4 || op == 6 || op == 9) {
            if (path)
                ref = manager.reference(target);
            if (op == 6) {
                pushJson(c, manager.resolve(ref).json());
                return 1;
            }
            if (op == 9) {
                ref = manager.resolve(ref);
                pushJson(c, {{"ref", ref.json()}, {"header", manager.descriptor(ref.path).json()}});
                return 1;
            }
            auto doc = manager.read(ref);
            pushJson(c, {{"ref", doc.ref.json()},
                         {"header", doc.header.json()},
                         {"encoding", doc.encoding == AssetManager::Encoding::Json   ? "json"
                                      : doc.encoding == AssetManager::Encoding::Text ? "text"
                                                                                     : "raw"}});
            if (doc.encoding == AssetManager::Encoding::Json)
                pushJson(c, Json::parse(doc.payload));
            else if (doc.encoding == AssetManager::Encoding::Text)
                duk_push_lstring(c, doc.payload.data(), doc.payload.size());
            else {
                auto* buffer = duk_push_fixed_buffer(c, doc.payload.size());
                std::memcpy(buffer, doc.payload.data(), doc.payload.size());
            }
            duk_put_prop_string(c, -2, "payload");
        } else {
            auto header = AssetHeader::fromJson(argumentJson(c, 1));
            std::string bytes;
            if (manager.encoding(header.type) == AssetManager::Encoding::Json)
                bytes = argumentJson(c, 2).dump();
            else {
                duk_size_t size;
                const char* data = duk_is_buffer_data(c, 2)
                                       ? static_cast<const char*>(duk_require_buffer_data(c, 2, &size))
                                       : duk_require_lstring(c, 2, &size);
                bytes.assign(data, size);
            }
            auto saved = path ? manager.write(target, header, bytes) : manager.write(ref, header, bytes);
            pushJson(c, saved.json());
        }
    } else if (op == 8) {
        duk_push_string(c, newPersistentId().c_str());
    } else if (op == 7) {
        manager.scan(duk_require_string(c, 0));
        return 0;
    }
    return 1;
}
static duk_ret_t contentCall(duk_context* c) {
    try {
        return contentDispatch(c);
    } catch (const std::exception& e) {
        duk_push_error_object(c, DUK_ERR_ERROR, "%s", e.what());
    }
    duk_throw_raw(c);
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
static TransformPose readTransformPose(duk_context* c, int idx, vec3 scale = vec3(1)) {
    idx = duk_normalize_index(c, idx);
    auto p = readPhysicsPose(c, idx);
    duk_get_prop_string(c, idx, "scale");
    if (duk_is_object(c, -1))
        scale = readVec(c, -1);
    duk_pop(c);
    return {p.position, p.rotation, scale};
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
    AssetReference,
    AcquireRenderTarget,
    RenderTargetInfo,
    ReleaseRenderTarget,
    ReadPixels,
    PollPixels,
    CancelPixels,
    SceneLoad,
    SceneSave,
    SceneData,
    SetSceneData,
    SceneObject,
    GetObjectPath,
    FindObject,
    CameraState,
    Log,
    Create,
    AddComponent,
    SetComponent,
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
    TransformScale,
    SetTransform,
    MoveBody,
    ShapeSweep,
    ShapeOverlap,
    Move,
    FindPath,
    SetCamera,
    SetSolid,
    SetMaterial,
    ReadJson,
    LightIntensity,
    ConfigureCollider,
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
        case AssetReference:
            pushJson(c, assets(c).reference(AssetPath(duk_require_string(c, 0))).json());
            return 1;
        case AcquireRenderTarget: {
            AssetRef ref;
            if (duk_is_string(c, 0))
                ref = assets(c).reference(AssetPath(duk_require_string(c, 0)));
            else {
                duk_dup(c, 0);
                duk_json_encode(c, -1);
                ref = AssetRef::fromJson(Json::parse(duk_require_string(c, -1)));
            }
            auto rt = w.renderTargets.acquire(assets(c).load<RenderTargetAsset>(ref));
            duk_push_string(c, rt->handle.c_str());
            return 1;
        }
        case RenderTargetInfo:
            pushJson(c, w.renderTargets.target(duk_require_string(c, 0))->info());
            return 1;
        case ReleaseRenderTarget:
            w.renderTargets.release(duk_require_string(c, 0));
            return 0;
        case ReadPixels: {
            PixelRegion region;
            std::string version;
            if (!duk_is_undefined(c, 1)) {
                duk_dup(c, 1);
                duk_json_encode(c, -1);
                auto j = Json::parse(duk_require_string(c, -1));
                if (j.contains("x"))
                    region.x = j.at("x").uint();
                if (j.contains("y"))
                    region.y = j.at("y").uint();
                if (j.contains("width"))
                    region.width = j.at("width").uint();
                if (j.contains("height"))
                    region.height = j.at("height").uint();
                if (j.contains("rtVersion"))
                    version = j.at("rtVersion").string();
            }
            auto request = w.renderTargets.read(duk_require_string(c, 0), region, version);
            runtime(c).trackPixelRead(request);
            duk_push_string(c, request.c_str());
            return 1;
        }
        case PollPixels: {
            auto request = w.renderTargets.request(duk_require_string(c, 0));
            auto result = request->take();
            if (!result) {
                duk_push_null(c);
                return 1;
            }
            auto j = textureContentsJson(result->contents);
            j["status"] = result->status;
            j["request"] = request->handle;
            j["target"] = request->target->handle;
            j["asset"] = request->target->asset->reference().json();
            j["assetGeneration"] = std::to_string(request->target->asset->generation);
            j["requestedTick"] = std::to_string(request->requestedTick);
            j["format"] = pixelFormatName(request->target->asset->format);
            j["region"] = Json{{"x", result->region.x},
                               {"y", result->region.y},
                               {"width", result->region.width},
                               {"height", result->region.height}};
            j["rowBytes"] = result->status == "ready"
                                ? result->region.width * pixelBytes(request->target->asset->format)
                                : 0u;
            pushJson(c, j);
            if (result->status == "ready") {
                const auto bytes = result->bytes.size();
                auto* buffer = duk_push_fixed_buffer(c, bytes);
                std::memcpy(buffer, result->bytes.data(), bytes);
                const auto format = request->target->asset->format;
                duk_uint_t type = format == PixelFormat::R32Uint ? DUK_BUFOBJ_UINT32ARRAY
                                  : format == PixelFormat::RGBA8 ? DUK_BUFOBJ_UINT8ARRAY
                                                                 : DUK_BUFOBJ_FLOAT32ARRAY;
                duk_push_buffer_object(c, -1, 0, bytes, type);
                duk_put_prop_string(c, -3, "data");
                duk_pop(c);
            }
            w.renderTargets.consumed(request->handle);
            runtime(c).forgetPixelRead(request->handle);
            return 1;
        }
        case CancelPixels: {
            std::string id = duk_require_string(c, 0);
            w.renderTargets.request(id)->cancel("cancelled");
            w.renderTargets.consumed(id);
            runtime(c).forgetPixelRead(id);
            return 0;
        }
        case Log:
            runtime(c).log(duk_safe_to_string(c, 0));
            return 0;
        case Create: {
            duk_dup(c, 0);
            duk_json_encode(c, -1);
            auto json = Json::parse(duk_require_string(c, -1));
            auto description = SceneEntity::fromJson(json);
            duk_pop(c);
            duk_push_uint(c, ScenePersistence::createEntity(w, description, assets(c),
                                                            !json.contains("persistent") ||
                                                                json.at("persistent").boolean()));
            return 1;
        }
        case SetComponent:
        case AddComponent: {
            Entity e = duk_require_uint(c, 0);
            std::string type = duk_require_string(c, 1);
            duk_dup(c, 2);
            duk_json_encode(c, -1);
            auto data = Json::parse(duk_require_string(c, -1));
            duk_pop(c);
            auto description = SceneEntity::fromJson({{"components", {{type, data}}}});
            if (duk_get_current_magic(c) == SetComponent)
                componentCatalog().update(w, e, type, description.components.values.at(type), assets(c));
            else
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
            auto value = type.inspect(w, e);
            if (!value)
                throw std::invalid_argument("Component is derived, not authored: " + type.name);
            if (type.resolveReferences)
                type.resolveReferences(*value, assets(c));
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
        case LocalTransform: {
            Entity e = duk_require_uint(c, 0);
            w.transforms.setLocal(e, readTransformPose(c, 1, w.get<Transform>(e).local.scale));
            return 0;
        }
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
            SceneEntity description;
            description.name = "Light";
            description.components.set(SceneTransform{{{num(c, 0), num(c, 1), num(c, 2)}}});
            description.components.set(
                LightComponent{{num(c, 4), num(c, 5), num(c, 6)}, num(c, 7), num(c, 3)});
            duk_push_uint(c, ScenePersistence::createEntity(w, description, assets(c)));
            return 1;
        }
        case GetPose: {
            const auto& e = w.get<Transform>(duk_require_uint(c, 0));
            pushVec(c, e.world.position);
            value(c, "yaw", e.yaw());
            pushVec(c, e.world.scale);
            duk_put_prop_string(c, -2, "scale");
            pushRotation(c, e.world.rotation);
            duk_put_prop_string(c, -2, "rotation");
            return 1;
        }
        case TransformScale:
            w.transforms.setScale(duk_require_uint(c, 0), readVec(c, 1));
            return 0;
        case SetTransform: {
            Entity e = duk_require_uint(c, 0);
            w.transforms.setWorld(e, readTransformPose(c, 1, w.get<Transform>(e).world.scale));
            return 0;
        }
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
        case SetCamera:
            w.resources.camera.target = {num(c, 0), num(c, 1), num(c, 2)};
            w.resources.camera.yaw = num(c, 3);
            w.resources.camera.pitch = num(c, 4);
            w.resources.camera.distance = num(c, 5);
            return 0;
        case SetSolid:
            w.motion.setSolid(duk_require_uint(c, 0), duk_get_boolean(c, 1) != 0);
            return 0;
        case SetMaterial: {
            AssetRef ref;
            if (duk_is_string(c, 1))
                ref = assets(c).reference(AssetPath(duk_require_string(c, 1)));
            else {
                duk_dup(c, 1);
                duk_json_encode(c, -1);
                ref = AssetRef::fromJson(Json::parse(duk_require_string(c, -1)));
                duk_pop(c);
            }
            w.render.setMaterial(duk_require_uint(c, 0), assets(c).load<MaterialAsset>(ref));
            return 0;
        }
        case ReadJson: {
            auto asset = assets(c).load<DataAsset>(AssetPath(duk_require_string(c, 0)));
            pushJson(c, asset->data);
            return 1;
        }
        case SceneLoad:
            runtime(c).host().requestScene(assets(c).reference(AssetPath(duk_require_string(c, 0))), true);
            return 0;
        case SceneSave: {
            auto ref = ScenePersistence::save(w, assets(c), AssetPath(duk_require_string(c, 0)),
                                              duk_require_string(c, 1));
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
        case FindObject: {
            ObjectPath path(duk_require_string(c, 0));
            path.map = assets(c).qualify(path.map);
            duk_push_uint(c, w.resolveObject(path));
            return 1;
        }
        case CameraState:
            pushJson(c, {{"yaw", w.resources.camera.yaw},
                         {"pitch", w.resources.camera.pitch},
                         {"distance", w.resources.camera.distance},
                         {"x", w.resources.camera.target.x},
                         {"z", w.resources.camera.target.z}});
            return 1;
        case LightIntensity:
            w.edit<LightComponent>(duk_require_uint(c, 0),
                                   [&](LightComponent& light) { light.intensity = num(c, 1); });
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
ScriptRuntime::ScriptRuntime(World& w, AssetManager& a, ui::UiCore* ui, RuntimeHost* host)
    : world_(w), assets_(a), host_(host), owner_(std::this_thread::get_id()), ui_(ui) {
    createContext();
}
RuntimeHost& ScriptRuntime::host() const {
    if (!host_)
        throw std::logic_error("This operation requires a RuntimeHost");
    return *host_;
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
                                {"asset", AssetReference, 1},
                                {"renderTarget", AcquireRenderTarget, 1},
                                {"renderTargetInfo", RenderTargetInfo, 1},
                                {"releaseRenderTarget", ReleaseRenderTarget, 1},
                                {"readPixels", ReadPixels, 2},
                                {"pollPixels", PollPixels, 1},
                                {"cancelPixels", CancelPixels, 1},
                                {"saveScene", SceneSave, 2},
                                {"sceneData", SceneData, 0},
                                {"setSceneData", SetSceneData, 1},
                                {"sceneObject", SceneObject, 1},
                                {"objectPath", GetObjectPath, 1},
                                {"findObject", FindObject, 1},
                                {"cameraState", CameraState, 0},
                                {"log", Log, 1},
                                {"create", Create, 1},
                                {"addComponent", AddComponent, 3},
                                {"setComponent", SetComponent, 3},
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
                                {"scale", TransformScale, 2},
                                {"transform", SetTransform, 2},
                                {"moveBody", MoveBody, 4},
                                {"physicsShapeSweep", ShapeSweep, 4},
                                {"physicsShapeOverlap", ShapeOverlap, 2},
                                {"move", Move, 3},
                                {"findPath", FindPath, 2},
                                {"camera", SetCamera, 6},
                                {"solid", SetSolid, 2},
                                {"setMaterial", SetMaterial, 2},
                                {"readJson", ReadJson, 1},
                                {"lightIntensity", LightIntensity, 2},
                                {"collider", ConfigureCollider, 2},
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
    duk_push_object(context_);
    const char* contentNames[] = {"mount", "unmount",   "mounts", "browse", "load",
                                  "save",  "reference", "scan",   "newId", "describe"};
    for (int i = 0; i < 10; ++i) {
        duk_push_c_function(context_, contentCall, DUK_VARARGS);
        duk_set_magic(context_, -1, i);
        duk_put_prop_string(context_, -2, contentNames[i]);
    }
    duk_put_prop_string(context_, -2, "content");
    duk_put_global_string(context_, "Engine");
    installRuntimeBindings(context_);
    if(host_)dynamicsBindings_=std::make_unique<dynamics::ScriptBindings>(context_,host_->dynamicsMailbox());
    if (ui_) {
        uiBindings_ = std::make_unique<UiBindings>(
            context_, *ui_, [this](const auto& text) { log(text); },
            [this](const auto& path) { return assets_.file(path).uri(); },
            [this](int args) {
                AssetManager::Scope scope(assets_, scriptOrigin_, false);
                return duk_pcall(context_, args);
            });
        uiBindings_->setVisible(hudEnabled_);
    }
}
ScriptRuntime::~ScriptRuntime() {
    dynamicsBindings_.reset();
    uiBindings_.reset();
    for (const auto& request : pixelReads_)
        world_.renderTargets.discard(request);
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
void ScriptRuntime::evaluateFile(const AssetRef& ref) {
    auto asset = assets_.load<ScriptAsset>(ref);
    AssetManager::Scope content(assets_, assets_.origin(ref), false);
    evaluateSource(asset->source, ref.path.string());
}
void ScriptRuntime::execute(const std::string& source, const std::string& label) {
    AssetManager::Scope content(assets_, scriptOrigin_, false);
    evaluateSource(source, label);
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
void ScriptRuntime::start(ContentSourceRef origin, const std::vector<AssetRef>& scripts) {
    if (started_)
        throw std::logic_error("A realm is started once; create a new realm to run a different program");
    started_ = true;
    scriptOrigin_ = std::move(origin);
    if (!scriptOrigin_ && !scripts.empty())
        scriptOrigin_ = assets_.origin(scripts.front());
    // One realm has one /Game origin, including callbacks invoked after top-level evaluation.
    for (const auto& script : scripts)
        if (assets_.origin(script) != scriptOrigin_)
            throw std::invalid_argument("Scripts in one realm must belong to the same Content");
    AssetManager::Scope content(assets_, scriptOrigin_, false);
    for (const auto& script : scripts)
        evaluateFile(script);
    notify("initialize");
    updateUi(0);
}
void ScriptRuntime::notify(const char* callback) {
    AssetManager::Scope content(assets_, scriptOrigin_, false);
    duk_get_global_string(context_, callback);
    if (duk_is_function(context_, -1))
        checkedCall(0);
    else
        duk_pop(context_);
}
void ScriptRuntime::updateUi(float dt) {
    AssetManager::Scope content(assets_, scriptOrigin_, false);
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
}
void ScriptRuntime::setHudEnabled(bool enabled) {
    hudEnabled_ = enabled;
    if (uiBindings_)
        uiBindings_->setVisible(enabled);
}
void ScriptRuntime::tick(float dt, const Input& rawInput, bool gameplayInput) {
    AssetManager::Scope content(assets_, scriptOrigin_, false);
    CpuScope inputScope("Input / Picking / JS Arguments");
    if (std::this_thread::get_id() != owner_)
        throw std::runtime_error("JS accessed outside engine thread");
    const Input& input = rawInput;
    bool captured = input.pointerCaptured;
    const auto* camera = host_ && host_->view.camera ? &*host_->view.camera : nullptr;
    auto picked = gameplayInput && !captured ? world_.pick(input.mouseX, input.mouseY, input, camera) : 0;
    auto ground =
        gameplayInput ? world_.groundAt(input.mouseX, input.mouseY, input, camera) : std::optional<vec3>{};
    duk_get_global_string(context_, "fixedUpdate");
    if (duk_is_undefined(context_, -1)) {
        duk_pop(context_);
        inputScope.finish();
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
    value(context_, "picked", picked);
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
}
} // namespace whimsical
