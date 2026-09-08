#include "SceneAsset.h"
#include "ecs/Components.h"
#include <set>
#include <stdexcept>

namespace afterlight {
static Json vector(vec3 v) {
    return Json::array({v.x, v.y, v.z});
}
static vec3 vector3(const Json& j) {
    if (j.elements().size() != 3)
        throw std::invalid_argument("Expected vec3");
    return {j.at(0).number(), j.at(1).number(), j.at(2).number()};
}
static vec4 vector4(const Json& j) {
    if (j.elements().size() != 4)
        throw std::invalid_argument("Expected vec4");
    return {j.at(0).number(), j.at(1).number(), j.at(2).number(), j.at(3).number()};
}
Json SceneEntity::json() const {
    return {{"id", id}, {"name", name}, {"enabled", enabled}, {"components", components.json()}};
}
SceneEntity SceneEntity::fromJson(const Json& item) {
    SceneEntity o;
    if (item.contains("id"))
        o.id = item.at("id").string();
    if (item.contains("name"))
        o.name = item.at("name").string();
    if (item.contains("enabled"))
        o.enabled = item.at("enabled").boolean();
    o.components = ComponentSet::fromJson(item.at("components"));
    return o;
}
// Version 3 is an import format only. Translate its mandatory recipe once at the
// serialization boundary; the runtime and every new save use optional components.
static Json migrateEntity(const Json& item) {
    Json c{{"transform", {{"position", item.at("position")}, {"rotation", item.at("rotation")}}},
           {"render", item.at("render")},
           {"collider", item.at("physics")}};
    if (item.at("interactable").boolean())
        c["interactable"] = Json::object();
    if (!item.at("animation").null()) {
        auto a = item.at("animation");
        c["animator"] = a;
        if (!a.at("mesh").null())
            c["skin"] = a.at("mesh");
    }
    if (!item.at("joints").elements().empty())
        c["joints"] = item.at("joints");
    if (!item.at("jointColliders").elements().empty())
        c["jointColliders"] = item.at("jointColliders");
    return {
        {"id", item.at("id")}, {"name", item.at("name")}, {"enabled", item.at("enabled")}, {"components", c}};
}
Json SceneResourceDescription::json() const {
    validate();
    Json j{{"scripts", Json::array()},
           {"materials", Json::array()},
           {"camera",
            {{"target", vector(camera.target)},
             {"yaw", camera.yaw},
             {"pitch", camera.pitch},
             {"distance", camera.distance},
             {"fov", camera.fov}}},
           {"navigation",
            {{"min", vector(navigation.min)},
             {"max", vector(navigation.max)},
             {"cellSize", navigation.cellSize},
             {"planeTolerance", navigation.planeTolerance}}},
           {"player", player},
           {"references", Json::object()},
           {"data", data}};
    for (const auto& script : scripts)
        j["scripts"].push(script.json());
    for (const auto& m : materials)
        j["materials"].push(m.json());
    j["materialAssets"] = Json::array();
    for (const auto& m : materialAssets)
        j["materialAssets"].push({{"index", m.first}, {"asset", m.second.json()}});
    for (const auto& r : references)
        j["references"][r.first] = r.second;
    return j;
}
Json SceneDocument::json() const {
    validate();
    auto j = SceneResourceDescription::json();
    j["version"] = 7;
    j["entities"] = Json::array();
    for (const auto& e : entities)
        j["entities"].push(e.json());
    return j;
}
SceneResourceDescription SceneResourceDescription::fromJson(const Json& j) {
    SceneResourceDescription s;
    for (const auto& script : j.at("scripts").elements())
        s.scripts.push_back(AssetRef::fromJson(script));
    for (const auto& m : j.at("materials").elements())
        s.materials.push_back(MaterialDefinition::fromJson(m));
    if (j.contains("materialAssets"))
        for (const auto& m : j.at("materialAssets").elements())
            s.materialAssets.emplace(m.at("index").uint(), AssetRef::fromJson(m.at("asset")));
    const auto& camera = j.at("camera");
    s.camera.target = vector3(camera.at("target"));
    s.camera.yaw = float(camera.at("yaw").number());
    s.camera.pitch = float(camera.at("pitch").number());
    s.camera.distance = float(camera.at("distance").number());
    s.camera.fov = float(camera.at("fov").number());
    const auto& nav = j.at("navigation");
    s.navigation.min = vector3(nav.at("min"));
    s.navigation.max = vector3(nav.at("max"));
    s.navigation.cellSize = float(nav.at("cellSize").number());
    s.navigation.planeTolerance = float(nav.at("planeTolerance").number());
    s.player = j.at("player").string();
    s.data = j.at("data");
    for (const auto& r : j.at("references").members())
        s.references[r.first] = r.second.string();
    s.validate();
    return s;
}
SceneDocument SceneDocument::fromJson(const Json& j) {
    auto version = j.at("version").uint();
    if (version < 3 || version > 7)
        throw std::invalid_argument("Unsupported Map version");
    SceneDocument s;
    static_cast<SceneResourceDescription&>(s) = SceneResourceDescription::fromJson(j);
    // Older maps import global lights as ordinary entities. New saves only use components.
    if (version < 6)
        for (const auto& l : j.at("lights").elements()) {
            auto p = vector4(l.at("positionRadius")), c = vector4(l.at("colorIntensity"));
            SceneEntity light;
            light.id = newPersistentId();
            light.name = "Light";
            light.components.set(SceneTransform{{vec3(p)}});
            light.components.set(LightComponent{vec3(c), c.w * (4 * Pi) * (c.x + c.y + c.z), p.w});
            s.entities.push_back(std::move(light));
        }
    for (const auto& item : j.at(version == 3 ? "objects" : "entities").elements()) {
        auto value = version == 3 ? migrateEntity(item) : item;
        // Legacy intensity was an inverse-square coefficient (W/sr). Convert at
        // the import boundary; v7 scripts and components always author radiant power.
        if (version < 7 && value.at("components").contains("light")) {
            auto& light = value["components"]["light"];
            float oldIntensity = light.contains("intensity") ? float(light.at("intensity").number()) : 1.f;
            auto color = light.contains("color") ? vector3(light.at("color")) : vec3(1);
            light["intensity"] = oldIntensity * (4 * Pi) * (color.x + color.y + color.z);
            light["type"] = "point";
        }
        if (version < 5) {
            auto fields = value.at("components").members();
            if (auto it = fields.find("jointColliders"); it != fields.end() && it->second.elements().empty())
                fields.erase(it);
            if (auto it = fields.find("animator"); it != fields.end()) {
                auto animation = it->second.members();
                bool root = !it->second.contains("rootMotion") || it->second.at("rootMotion").boolean();
                animation.erase("rootMotion");
                it->second = Json(std::move(animation));
                if (root)
                    fields["rootMotion"] = {{"mode", "grounded"}, {"preserveAnchor", true}};
            }
            value["components"] = Json(std::move(fields));
        }
        s.entities.push_back(SceneEntity::fromJson(value));
    }
    s.validate();
    return s;
}
static bool finite(vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
void SceneDocument::validate() const {
    SceneResourceDescription::validate();
    std::map<std::string, const SceneEntity*> ids;
    for (const auto& o : entities) {
        validatePersistentId(o.id);
        if (!ids.emplace(o.id, &o).second)
            throw std::invalid_argument("Duplicate entity ID");
        componentCatalog().validate(o.components);
        if (auto r = o.components.find<SceneRender>(); r && r->appearance.material >= materials.size())
            throw std::invalid_argument("Invalid material index");
    }
    for (const auto& o : entities) {
        std::set<std::string> chain{o.id};
        auto t = o.components.find<SceneTransform>();
        while (t && !t->parent.empty()) {
            if (!chain.insert(t->parent).second)
                throw std::invalid_argument("Transform hierarchy cycle");
            auto p = ids.find(t->parent);
            if (p == ids.end() || !p->second->components.find<SceneTransform>())
                throw std::invalid_argument("Missing parent Transform");
            t = p->second->components.find<SceneTransform>();
        }
    }
    if (!player.empty() && !ids.count(player))
        throw std::invalid_argument("Map player references missing Object");
    for (const auto& r : references)
        if (!ids.count(r.second))
            throw std::invalid_argument("Map reference targets missing Object: " + r.first);
}
void SceneResourceDescription::validate() const {
    for (const auto& m : materialAssets)
        if (m.first >= materials.size())
            throw std::invalid_argument("Material asset index outside Map material table");
    if (!finite(navigation.min) || !finite(navigation.max) || !std::isfinite(navigation.cellSize) ||
        navigation.cellSize < .05f || !std::isfinite(navigation.planeTolerance) ||
        navigation.planeTolerance < 0 || glm::any(glm::lessThanEqual(navigation.max, navigation.min)))
        throw std::invalid_argument("Invalid Map navigation");
    if (!finite(camera.target) || !std::isfinite(camera.yaw) || !std::isfinite(camera.pitch) ||
        !std::isfinite(camera.distance) || camera.distance <= 0 || !std::isfinite(camera.fov) ||
        camera.fov <= 0 || camera.fov >= 3.14f)
        throw std::invalid_argument("Invalid Map camera");
    (void)data.members();
}
} // namespace afterlight
