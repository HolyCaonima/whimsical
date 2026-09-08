#include "RuntimeBindings.h"
#include "RuntimeHost.h"
#include "scene/ScenePersistence.h"

namespace afterlight {
namespace {
template <class T> T& stored(duk_context* c, const char* name) {
    duk_push_heap_stash(c);
    duk_get_prop_string(c, -1, name);
    auto* value = static_cast<T*>(duk_get_pointer(c, -1));
    duk_pop_2(c);
    return *value;
}
Json json(duk_context* c, int index) {
    duk_dup(c, index);
    duk_json_encode(c, -1);
    auto value = Json::parse(duk_require_string(c, -1));
    duk_pop(c);
    return value;
}
void push(duk_context* c, const Json& value) {
    auto text = value.dump();
    duk_push_lstring(c, text.data(), text.size());
    duk_json_decode(c, -1);
}
AssetRef reference(duk_context* c, AssetManager& assets, int index) {
    return duk_is_string(c, index) ? assets.reference(AssetPath(duk_require_string(c, index)))
                                   : assets.resolve(AssetRef::fromJson(json(c, index)));
}
Json rectJson(const ViewRect& r) {
    return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}};
}
Json cameraJson(const Camera& camera) {
    SceneResourceDescription document;
    document.camera = camera;
    return document.json().at("camera");
}
enum Op {
    Load,
    Capture,
    Restore,
    Save,
    Resources,
    Info,
    Play,
    Stop,
    Pause,
    SimulationState,
    ViewGet,
    ViewSet,
    ViewReset,
    ViewPixel,
    ViewOutlines,
    EntityInfo,
    Rename,
    Persistent,
    FindEntity,
    ComponentTypes,
    Components,
    ProjectRead,
    OpenFileDialog
};
// Duktape reports errors only after the C++ owners in dispatch have unwound.
duk_ret_t dispatch(duk_context* c) {
    auto& world = stored<World>(c, "world");
    auto& assets = stored<AssetManager>(c, "assets");
    auto op = Op(duk_get_current_magic(c));
    if (op == ProjectRead) {
        Project project(std::filesystem::u8path(duk_require_string(c, 0)));
        Json scripts = Json::array(), hostScripts = Json::array();
        for (const auto& path : project.scripts())
            scripts.push(path.string());
        for (const auto& path : project.hostScripts())
            hostScripts.push(path.string());
        push(c, {{"path", (project.root() / ".project").u8string()},
                 {"name", project.name()}, {"id", project.id()},
                 {"content", project.content().u8string()},
                 {"startupMap", project.startupMap().string()}, {"scripts", scripts},
                 {"hostScripts", hostScripts}, {"startupMode", project.runOnStartup() ? "run" : "load"}});
        return 1;
    }
    if (op == OpenFileDialog) {
        auto& host = stored<ScriptRuntime>(c, "runtime").host();
        auto value = json(c, 0);
        OpenFileDialogOptions options;
        if (value.contains("title")) options.title = value.at("title").string();
        if (value.contains("initialDirectory")) options.initialDirectory = value.at("initialDirectory").string();
        if (value.contains("filters"))
            for (const auto& filter : value.at("filters").elements())
                options.filters.push_back({filter.at("name").string(), filter.at("pattern").string()});
        auto path = host.openFileDialog(options);
        if (path) duk_push_string(c, path->c_str());
        else duk_push_null(c);
        return 1;
    }
    if (op == EntityInfo || op == Components) {
        auto e = duk_require_uint(c, 0);
        const auto& id = world.get<Identity>(e);
        auto authored = componentCatalog().capture(world, e, assets).json();
        Json derived = Json::array();
        for (const auto& [name, contract] : componentCatalog().entries())
            if (contract.present(world.registry(), e) && !authored.contains(name))
                derived.push(name);
        if (op == Components)
            push(c, {{"authored", authored}, {"derived", derived}});
        else
            push(c, {{"entity", e},
                     {"id", id.persistentId},
                     {"name", id.name},
                     {"enabled", !world.has<Disabled>(e)},
                     {"effectiveEnabled", world.enabled(e)},
                     {"persistent", id.persistent},
                     {"effectivePersistent", world.persistent(e)},
                     {"components", authored},
                     {"derived", derived}});
        return 1;
    }
    if (op == Rename) {
        world.rename(duk_require_uint(c, 0), duk_require_string(c, 1));
        return 0;
    }
    if (op == Persistent) {
        world.setPersistent(duk_require_uint(c, 0), duk_require_boolean(c, 1) != 0);
        return 0;
    }
    if (op == FindEntity) {
        duk_push_uint(c, world.findObject(duk_require_string(c, 0)));
        return 1;
    }
    if (op == ComponentTypes) {
        Json types = Json::array();
        for (const auto& [name, contract] : componentCatalog().entries()) {
            Json dependencies = Json::array(), owns = Json::array();
            auto required = contract.dependencies;
            if (duk_is_number(c, 0)) {
                auto e = duk_require_uint(c, 0);
                world.registry().require(e);
                if (contract.present(world.registry(), e) && contract.runtimeDependencies) {
                    auto extra = contract.runtimeDependencies(world, e);
                    required.insert(required.end(), extra.begin(), extra.end());
                }
            }
            for (const auto& dependency : required)
                dependencies.push(
                    {{"name", dependency.name},
                     {"onRemoval",
                      dependency.removal == OnDependencyRemoval::Cascade ? "cascade" : "reject"}});
            for (const auto& owned : contract.owns)
                owns.push(owned);
            types.push({{"name", name}, {"dependencies", dependencies}, {"owns", owns}});
        }
        push(c, types);
        return 1;
    }
    if (op == Resources) {
        if (duk_get_top(c) && !duk_is_undefined(c, 0))
            ScenePersistence::setResources(world, assets, json(c, 0));
        push(c, ScenePersistence::resources(world, assets));
        return 1;
    }
    auto& host = stored<ScriptRuntime>(c, "runtime").host();
    switch (op) {
    case Load:
        host.requestScene(reference(c, assets, 0), false);
        return 0;
    case Capture:
        push(c, {{"source", world.mapAsset().id.empty() ? Json() : world.mapAsset().json()},
                 {"document", host.captureScene().json()}});
        return 1;
    case Restore: {
        auto snapshot = json(c, 0);
        auto source = snapshot.at("source").null()
                          ? AssetRef{}
                          : assets.resolve(AssetRef::fromJson(snapshot.at("source")));
        host.requestRestore(SceneDocument::fromJson(snapshot.at("document")), source);
        return 0;
    }
    case Save: {
        auto path = duk_is_string(c, 0) ? AssetPath(duk_require_string(c, 0)) : reference(c, assets, 0).path;
        push(c, host.saveScene(path, duk_require_string(c, 1)).json());
        return 1;
    }
    case Info:
        push(c, {{"source", world.mapAsset().id.empty() ? Json() : world.mapAsset().json()}});
        return 1;
    case Play: {
        std::optional<std::vector<AssetRef>> scripts;
        if (duk_get_top(c) && !duk_is_undefined(c, 0)) {
            scripts.emplace();
            duk_get_prop_string(c, 0, "scripts");
            if (!duk_is_array(c, -1))
                throw std::invalid_argument("play scripts must be an array of asset references");
            for (duk_uarridx_t i = 0; i < duk_get_length(c, -1); ++i) {
                duk_get_prop_index(c, -1, i);
                scripts->push_back(reference(c, assets, -1));
                duk_pop(c);
            }
            duk_pop(c);
        }
        host.requestPlay(std::move(scripts));
        return 0;
    }
    case Stop:
        host.requestStop();
        return 0;
    case Pause:
        host.requestPause(duk_require_boolean(c, 0) != 0);
        return 0;
    case SimulationState:
        push(c, {{"running", host.running()}, {"paused", host.paused()}, {"time", host.simulationTime()}});
        return 1;
    case ViewOutlines: {
        std::vector<EntityOutline> next;
        auto request = json(c, 0);
        for (const auto& item : request.elements()) {
            auto entity = item.at("entity").uint();
            world.registry().require(entity);
            const auto& rgba = item.at("color").elements();
            if (rgba.size() != 4)
                throw std::invalid_argument("Outline color requires RGBA");
            vec4 color;
            for (int i = 0; i < 4; ++i) {
                color[i] = float(rgba[i].number());
                if (!std::isfinite(color[i]) || color[i] < 0 || color[i] > 1)
                    throw std::invalid_argument("Outline color must be in [0,1]");
            }
            next.push_back({entity, color});
        }
        stored<ScriptRuntime>(c, "runtime").outlines = std::move(next);
        return 0;
    }
    case ViewSet: {
        auto value = json(c, 0);
        auto next = host.view;
        if (value.contains("rectangle")) {
            const auto& r = value.at("rectangle");
            next.rectangle = {r.at("x").uint(), r.at("y").uint(), r.at("width").uint(),
                              r.at("height").uint()};
            if ((!next.rectangle.width != !next.rectangle.height) ||
                (!next.rectangle.width && (next.rectangle.x || next.rectangle.y)))
                throw std::invalid_argument("View rectangle must have positive dimensions or be all zero");
        }
        if (value.contains("camera")) {
            if (value.at("camera").null())
                next.camera.reset();
            else {
                auto document = SceneResourceDescription{}.json();
                document["camera"] = value.at("camera");
                next.camera = SceneResourceDescription::fromJson(document).camera;
            }
        }
        host.view = std::move(next);
        return 0;
    }
    case ViewGet:
        push(c, {{"rectangle", rectJson(host.view.rectangle)},
                 {"camera", cameraJson(host.view.camera ? *host.view.camera : world.resources.camera)},
                 {"cameraOverride", bool(host.view.camera)}});
        return 1;
    case ViewReset:
        host.view = {};
        return 0;
    case ViewPixel: {
        // Explicit surface and target sizes keep this mapping useful for fixed-size ID RTs too.
        auto r = host.view.rectangle.fit(duk_require_uint(c, 2), duk_require_uint(c, 3));
        auto x = duk_require_number(c, 0), y = duk_require_number(c, 1);
        if (!r.contains(float(x), float(y))) {
            duk_push_null(c);
            return 1;
        }
        auto w = duk_get_uint_default(c, 4, r.width), h = duk_get_uint_default(c, 5, r.height);
        if (!w || !h)
            throw std::invalid_argument("Pixel target size must be positive");
        push(c, {{"x", uint32_t((x - r.x) * w / r.width)}, {"y", uint32_t((y - r.y) * h / r.height)}});
        return 1;
    }
    default:
        throw std::logic_error("Unknown runtime operation");
    }
}
duk_ret_t call(duk_context* c) {
    try {
        return dispatch(c);
    } catch (const std::exception& error) {
        duk_push_error_object(c, DUK_ERR_ERROR, "%s", error.what());
    }
    duk_throw_raw(c);
}
void bind(duk_context* c, const char* name, Op op) {
    duk_push_c_function(c, call, DUK_VARARGS);
    duk_set_magic(c, -1, op);
    duk_put_prop_string(c, -2, name);
}
} // namespace
void installRuntimeBindings(duk_context* c) {
    duk_get_global_string(c, "Engine");
    duk_push_object(c);
    bind(c, "read", ProjectRead);
    duk_put_prop_string(c, -2, "project");
    duk_push_object(c);
    bind(c, "openDialog", OpenFileDialog);
    duk_put_prop_string(c, -2, "files");
    bind(c, "entity", EntityInfo);
    bind(c, "rename", Rename);
    bind(c, "persistent", Persistent);
    bind(c, "findEntity", FindEntity);
    bind(c, "componentTypes", ComponentTypes);
    bind(c, "components", Components);
    duk_push_object(c);
    bind(c, "load", Load);
    bind(c, "capture", Capture);
    bind(c, "restore", Restore);
    bind(c, "save", Save);
    bind(c, "resources", Resources);
    bind(c, "info", Info);
    duk_put_prop_string(c, -2, "scene");
    duk_push_object(c);
    bind(c, "play", Play);
    bind(c, "stop", Stop);
    bind(c, "pause", Pause);
    bind(c, "state", SimulationState);
    duk_put_prop_string(c, -2, "simulation");
    duk_push_object(c);
    bind(c, "get", ViewGet);
    bind(c, "set", ViewSet);
    bind(c, "outlines", ViewOutlines);
    bind(c, "reset", ViewReset);
    bind(c, "pixel", ViewPixel);
    duk_put_prop_string(c, -2, "view");
    duk_pop(c);
}
} // namespace afterlight
