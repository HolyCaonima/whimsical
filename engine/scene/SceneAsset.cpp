#include "SceneAsset.h"
#include <set>
#include <stdexcept>

namespace afterlight {
static Json vector(vec3 v) {
    return Json::array({v.x, v.y, v.z});
}
static Json vector(vec4 v) {
    return Json::array({v.x, v.y, v.z, v.w});
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
static Json pose(const PhysicsPose& p) {
    return {{"position", vector(p.position)},
            {"rotation", vector(vec4(p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w))}};
}
static PhysicsPose pose(const Json& j) {
    auto q = vector4(j.at("rotation"));
    if (std::abs(glm::length(q) - 1) > .001f)
        throw std::invalid_argument("Expected unit quaternion");
    return {vector3(j.at("position")), quat(q.w, q.x, q.y, q.z)};
}
static Json shape(const ColliderShape& s) {
    if (s.type == ColliderType::Box)
        return {{"type", "box"}, {"halfExtents", vector(s.halfExtents)}};
    return {{"type", "capsule"}, {"radius", s.radius}, {"height", s.height()}};
}
static ColliderShape shape(const Json& j) {
    if (j.at("type").string() == "box")
        return ColliderShape::box(vector3(j.at("halfExtents")));
    if (j.at("type").string() == "capsule")
        return ColliderShape::capsule(float(j.at("radius").number()), float(j.at("height").number()));
    throw std::invalid_argument("Unknown collider type");
}
Json SceneDocument::json() const {
    validate();
    Json j{{"version", 1},
           {"scripts", Json::array()},
           {"objects", Json::array()},
           {"materials", Json::array()},
           {"lights", Json::array()},
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
        j["materials"].push({{"albedoRoughness", vector(m.albedoRoughness)},
                             {"emissionMetallic", vector(m.emissionMetallic)}});
    for (const auto& l : lights)
        j["lights"].push(
            {{"positionRadius", vector(l.positionRadius)}, {"colorIntensity", vector(l.colorIntensity)}});
    for (const auto& r : references)
        j["references"][r.first] = r.second;
    for (const auto& o : objects) {
        Json item{{"id", o.id},
                  {"name", o.name},
                  {"position", vector(o.position)},
                  {"yaw", o.yaw},
                  {"enabled", o.enabled},
                  {"interactable", o.interactable},
                  {"render",
                   {{"shape", o.render.shape == Shape::Box ? "box" : "capsule"},
                    {"scale", vector(o.render.scale)},
                    {"offset", vector(o.render.offset)},
                    {"animationScale", vector(o.render.animationScale)},
                    {"material", o.render.material},
                    {"visible", o.render.visible}}},
                  {"physics",
                   {{"shape", shape(o.collider)},
                    {"motion", o.motion == BodyMotion::Static ? "static" : "kinematic"},
                    {"layer", o.layer},
                    {"blocking", o.blocking},
                    {"walkable", o.walkable},
                    {"pickable", o.pickable}}},
                  {"animation", Json()},
                  {"joints", Json::array()},
                  {"jointColliders", Json::array()}};
        if (o.animation) {
            const auto& a = *o.animation;
            Json attributes = Json::object();
            for (const auto& v : a.attributes)
                attributes[v.first] = v.second;
            item["animation"] = {{"asset", a.asset.json()},
                                 {"mesh", a.mesh ? a.mesh->json() : Json()},
                                 {"rootMotion", a.rootMotion},
                                 {"rootOffset", vector(a.rootOffset)},
                                 {"attributes", attributes}};
        }
        for (const auto& p : o.joints)
            item["joints"].push(pose(p));
        for (const auto& c : o.jointColliders)
            item["jointColliders"].push({{"joint", c.joint},
                                         {"shape", shape(c.shape)},
                                         {"local", pose(c.local)},
                                         {"blocking", c.blocking}});
        j["objects"].push(std::move(item));
    }
    return j;
}
SceneDocument SceneDocument::fromJson(const Json& j) {
    if (j.at("version").uint() != 1)
        throw std::invalid_argument("Unsupported Map version");
    SceneDocument s;
    for (const auto& script : j.at("scripts").elements())
        s.scripts.push_back(AssetRef::fromJson(script));
    for (const auto& m : j.at("materials").elements())
        s.materials.push_back({vector4(m.at("albedoRoughness")), vector4(m.at("emissionMetallic"))});
    for (const auto& l : j.at("lights").elements())
        s.lights.push_back({vector4(l.at("positionRadius")), vector4(l.at("colorIntensity"))});
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
    for (const auto& item : j.at("objects").elements()) {
        SceneObject o;
        o.id = item.at("id").string();
        o.name = item.at("name").string();
        o.position = vector3(item.at("position"));
        o.yaw = float(item.at("yaw").number());
        o.enabled = item.at("enabled").boolean();
        o.interactable = item.at("interactable").boolean();
        const auto& r = item.at("render");
        auto kind = r.at("shape").string();
        if (kind != "box" && kind != "capsule")
            throw std::invalid_argument("Unknown render shape");
        o.render.shape = kind == "box" ? Shape::Box : Shape::Capsule;
        o.render.scale = vector3(r.at("scale"));
        o.render.offset = vector3(r.at("offset"));
        o.render.animationScale = vector3(r.at("animationScale"));
        o.render.material = r.at("material").uint();
        o.render.visible = r.at("visible").boolean();
        const auto& p = item.at("physics");
        o.collider = shape(p.at("shape"));
        auto motion = p.at("motion").string();
        if (motion != "static" && motion != "kinematic")
            throw std::invalid_argument("Unknown body motion");
        o.motion = motion == "static" ? BodyMotion::Static : BodyMotion::Kinematic;
        o.layer = p.at("layer").uint();
        o.blocking = p.at("blocking").boolean();
        o.walkable = p.at("walkable").boolean();
        o.pickable = p.at("pickable").boolean();
        const auto& a = item.at("animation");
        if (!a.null()) {
            SceneAnimation animation;
            animation.asset = AssetRef::fromJson(a.at("asset"));
            if (!a.at("mesh").null())
                animation.mesh = AssetRef::fromJson(a.at("mesh"));
            animation.rootMotion = a.at("rootMotion").boolean();
            animation.rootOffset = vector3(a.at("rootOffset"));
            for (const auto& v : a.at("attributes").members())
                animation.attributes[v.first] = v.second.string();
            o.animation = std::move(animation);
        }
        for (const auto& jointPose : item.at("joints").elements())
            o.joints.push_back(pose(jointPose));
        for (const auto& c : item.at("jointColliders").elements())
            o.jointColliders.push_back({c.at("joint").uint(), shape(c.at("shape")), pose(c.at("local")),
                                        c.at("blocking").boolean()});
        s.objects.push_back(std::move(o));
    }
    s.validate();
    return s;
}
static bool finite(vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
void SceneDocument::validate() const {
    std::set<std::string> ids;
    for (const auto& o : objects) {
        validatePersistentId(o.id);
        if (!ids.insert(o.id).second)
            throw std::invalid_argument("Duplicate Map Object ID");
        if (o.render.material >= materials.size() || !finite(o.position) || !std::isfinite(o.yaw) ||
            !finite(o.render.scale) || !finite(o.render.offset) || !finite(o.render.animationScale) ||
            glm::any(glm::lessThanEqual(o.render.scale, vec3(0))) ||
            glm::any(glm::lessThanEqual(o.render.animationScale, vec3(0))))
            throw std::invalid_argument("Invalid Map render component");
        if (o.animation && !o.joints.empty())
            throw std::invalid_argument("Solved joint poses are runtime state");
    }
    if (!player.empty() && !ids.count(player))
        throw std::invalid_argument("Map player references missing Object");
    for (const auto& r : references)
        if (!ids.count(r.second))
            throw std::invalid_argument("Map reference targets missing Object: " + r.first);
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
