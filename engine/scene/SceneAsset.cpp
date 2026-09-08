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
Json SceneResourceDescription::json() const {
    validate();
    Json j{{"scripts", Json::array()},
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
           {"references", Json::object()},
           {"data", data}};
    for (const auto& script : scripts)
        j["scripts"].push(script.json());
    for (const auto& r : references)
        j["references"][r.first] = r.second;
    return j;
}
Json SceneDocument::json() const {
    validate();
    auto j = SceneResourceDescription::json();
    j["version"] = 11;
    j["entities"] = Json::array();
    for (const auto& e : entities)
        j["entities"].push(e.json());
    return j;
}
SceneResourceDescription SceneResourceDescription::fromJson(const Json& j) {
    SceneResourceDescription s;
    for (const auto& script : j.at("scripts").elements())
        s.scripts.push_back(AssetRef::fromJson(script));
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
    s.data = j.at("data");
    for (const auto& r : j.at("references").members())
        s.references[r.first] = r.second.string();
    s.validate();
    return s;
}
SceneDocument SceneDocument::fromJson(const Json& j) {
    if (j.at("version").uint() != 11)
        throw std::invalid_argument("Map requires Transform TRS (v11); migrate with tools/migrate_transform_trs.py");
    SceneDocument s;
    static_cast<SceneResourceDescription&>(s) = SceneResourceDescription::fromJson(j);
    for (const auto& item : j.at("entities").elements())
        s.entities.push_back(SceneEntity::fromJson(item));
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
    for (const auto& r : references)
        if (!ids.count(r.second))
            throw std::invalid_argument("Map reference targets missing Object: " + r.first);
}
void SceneResourceDescription::validate() const {
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
