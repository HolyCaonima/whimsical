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

Json SceneEntity::json() const {
    Json c = Json::object();
    if (transform) {
        c["transform"] = pose(transform->local);
        c["transform"]["parent"] = transform->parent;
    }
    if (render) {
        const auto& r = render->appearance;
        c["render"] = {{"shape", r.shape == Shape::Box ? "box" : "capsule"},
                       {"scale", vector(r.scale)},
                       {"offset", vector(r.offset)},
                       {"animationScale", vector(r.animationScale)},
                       {"material", r.material},
                       {"visible", r.visible}};
        if (render->mesh)
            c["render"]["mesh"] = render->mesh->json();
    }
    if (collider) {
        const auto& p = *collider;
        c["collider"] = {
            {"shape", shape(p.shape)}, {"motion", p.motion == BodyMotion::Static ? "static" : "kinematic"},
            {"layer", p.layer},        {"blocking", p.blocking},
            {"walkable", p.walkable},  {"pickable", p.pickable}};
    }
    if (interactable)
        c["interactable"] = Json::object();
    if (animation) {
        const auto& a = *animation;
        Json attributes = Json::object();
        for (const auto& v : a.attributes)
            attributes[v.first] = v.second;
        c["animator"] = {{"asset", a.asset.json()},
                         {"rootMotion", a.rootMotion},
                         {"rootOffset", vector(a.rootOffset)},
                         {"attributes", attributes}};
    }
    if (skin)
        c["skin"] = skin->json();
    if (data)
        c["data"] = *data;
    if (joints) {
        c["joints"] = Json::array();
        for (const auto& p : *joints)
            c["joints"].push(pose(p));
    }
    if (!jointColliders.empty()) {
        c["jointColliders"] = Json::array();
        for (const auto& p : jointColliders)
            c["jointColliders"].push({{"joint", p.joint},
                                      {"shape", shape(p.shape)},
                                      {"local", pose(p.local)},
                                      {"blocking", p.blocking}});
    }
    return {{"id", id}, {"name", name}, {"enabled", enabled}, {"components", c}};
}
SceneEntity SceneEntity::fromJson(const Json& item) {
    SceneEntity o;
    if (item.contains("id"))
        o.id = item.at("id").string();
    if (item.contains("name"))
        o.name = item.at("name").string();
    if (item.contains("enabled"))
        o.enabled = item.at("enabled").boolean();
    const auto& c = item.at("components");
    const std::set<std::string> known{"transform", "render", "collider",       "interactable", "animator",
                                      "skin",      "joints", "jointColliders", "data"};
    for (const auto& entry : c.members())
        if (!known.count(entry.first))
            throw std::invalid_argument("Unknown component: " + entry.first);
    if (c.contains("transform")) {
        const auto& t = c.at("transform");
        PhysicsPose p;
        if (t.contains("position"))
            p.position = vector3(t.at("position"));
        if (t.contains("rotation")) {
            auto q = vector4(t.at("rotation"));
            p.rotation = quat(q.w, q.x, q.y, q.z);
        }
        o.transform = SceneTransform{p, t.contains("parent") ? t.at("parent").string() : ""};
    }
    if (c.contains("render")) {
        const auto& r = c.at("render");
        SceneRender render;
        if (r.contains("mesh"))
            render.mesh = AssetRef::fromJson(r.at("mesh"));
        auto& a = render.appearance;
        if (r.contains("shape")) {
            auto kind = r.at("shape").string();
            if (kind != "box" && kind != "capsule")
                throw std::invalid_argument("Unknown render shape");
            a.shape = kind == "box" ? Shape::Box : Shape::Capsule;
        }
        if (r.contains("scale"))
            a.scale = vector3(r.at("scale"));
        if (r.contains("offset"))
            a.offset = vector3(r.at("offset"));
        if (r.contains("animationScale"))
            a.animationScale = vector3(r.at("animationScale"));
        if (r.contains("material"))
            a.material = r.at("material").uint();
        if (r.contains("visible"))
            a.visible = r.at("visible").boolean();
        o.render = std::move(render);
    }
    if (c.contains("collider")) {
        const auto& p = c.at("collider");
        SceneCollider collider;
        collider.shape = shape(p.at("shape"));
        if (p.contains("motion")) {
            auto motion = p.at("motion").string();
            if (motion != "static" && motion != "kinematic")
                throw std::invalid_argument("Unknown body motion");
            collider.motion = motion == "static" ? BodyMotion::Static : BodyMotion::Kinematic;
        }
        if (p.contains("layer"))
            collider.layer = p.at("layer").uint();
        if (p.contains("blocking"))
            collider.blocking = p.at("blocking").boolean();
        if (p.contains("walkable"))
            collider.walkable = p.at("walkable").boolean();
        if (p.contains("pickable"))
            collider.pickable = p.at("pickable").boolean();
        o.collider = collider;
    }
    o.interactable = c.contains("interactable");
    if (c.contains("animator")) {
        const auto& a = c.at("animator");
        SceneAnimation animation;
        animation.asset = AssetRef::fromJson(a.at("asset"));
        if (a.contains("rootMotion"))
            animation.rootMotion = a.at("rootMotion").boolean();
        if (a.contains("rootOffset"))
            animation.rootOffset = vector3(a.at("rootOffset"));
        if (a.contains("attributes"))
            for (const auto& v : a.at("attributes").members())
                animation.attributes[v.first] = v.second.string();
        o.animation = std::move(animation);
    }
    if (c.contains("skin"))
        o.skin = AssetRef::fromJson(c.at("skin"));
    if (c.contains("data")) {
        (void)c.at("data").members();
        o.data = c.at("data");
    }
    if (c.contains("joints")) {
        o.joints.emplace();
        for (const auto& p : c.at("joints").elements())
            o.joints->push_back(pose(p));
    }
    if (c.contains("jointColliders"))
        for (const auto& p : c.at("jointColliders").elements())
            o.jointColliders.push_back({p.at("joint").uint(), shape(p.at("shape")), pose(p.at("local")),
                                        p.at("blocking").boolean()});
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
    c["jointColliders"] = item.at("jointColliders");
    return {
        {"id", item.at("id")}, {"name", item.at("name")}, {"enabled", item.at("enabled")}, {"components", c}};
}
Json SceneDocument::json() const {
    validate();
    Json j{{"version", 4},
           {"scripts", Json::array()},
           {"entities", Json::array()},
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
        j["materials"].push(m.json());
    j["materialAssets"] = Json::array();
    for (const auto& m : materialAssets)
        j["materialAssets"].push({{"index", m.first}, {"asset", m.second.json()}});
    for (const auto& l : lights)
        j["lights"].push(
            {{"positionRadius", vector(l.positionRadius)}, {"colorIntensity", vector(l.colorIntensity)}});
    for (const auto& r : references)
        j["references"][r.first] = r.second;
    for (const auto& e : entities)
        j["entities"].push(e.json());
    return j;
}
SceneDocument SceneDocument::fromJson(const Json& j) {
    auto version = j.at("version").uint();
    if (version != 3 && version != 4)
        throw std::invalid_argument("Unsupported Map version");
    SceneDocument s;
    for (const auto& script : j.at("scripts").elements())
        s.scripts.push_back(AssetRef::fromJson(script));
    for (const auto& m : j.at("materials").elements())
        s.materials.push_back(MaterialDefinition::fromJson(m));
    if (j.contains("materialAssets"))
        for (const auto& m : j.at("materialAssets").elements())
            s.materialAssets.emplace(m.at("index").uint(), AssetRef::fromJson(m.at("asset")));
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
    for (const auto& item : j.at(version == 3 ? "objects" : "entities").elements())
        s.entities.push_back(SceneEntity::fromJson(version == 3 ? migrateEntity(item) : item));
    s.validate();
    return s;
}
static bool finite(vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
void SceneDocument::validate() const {
    for (const auto& m : materialAssets)
        if (m.first >= materials.size())
            throw std::invalid_argument("Material asset index outside Map material table");

    std::map<std::string, const SceneEntity*> ids;
    for (const auto& o : entities) {
        validatePersistentId(o.id);
        if (!ids.emplace(o.id, &o).second)
            throw std::invalid_argument("Duplicate entity ID");
        if ((o.render || o.collider || o.animation || o.joints) && !o.transform)
            throw std::invalid_argument("Spatial components require Transform");
        if (o.transform) {
            const auto& t = o.transform->local;
            auto length = glm::length(t.rotation);
            if (!finite(t.position) || !std::isfinite(length) || std::abs(length - 1) > .001f)
                throw std::invalid_argument("Invalid entity transform");
        }
        if (o.render) {
            const auto& r = o.render->appearance;
            if (r.material >= materials.size() || !finite(r.scale) || !finite(r.offset) ||
                !finite(r.animationScale) || glm::any(glm::lessThanEqual(r.scale, vec3(0))) ||
                glm::any(glm::lessThanEqual(r.animationScale, vec3(0))))
                throw std::invalid_argument("Invalid render component");
        }
        if (o.animation && o.joints)
            throw std::invalid_argument("Solved poses are runtime state");
        if (o.data)
            (void)o.data->members();
        if (o.animation && o.animation->rootMotion &&
            (!o.collider || o.collider->shape.type != ColliderType::Capsule))
            throw std::invalid_argument("Root motion requires capsule collider");
        if (o.skin && (!o.animation || !o.render || o.render->mesh))
            throw std::invalid_argument("Skin requires Animator and Renderable without static geometry");
        if (!o.jointColliders.empty() && !o.animation && (!o.joints || o.joints->empty()))
            throw std::invalid_argument("Joint colliders require joint poses");
    }
    for (const auto& o : entities) {
        std::set<std::string> chain{o.id};
        auto t = o.transform ? &*o.transform : nullptr;
        while (t && !t->parent.empty()) {
            if (!chain.insert(t->parent).second)
                throw std::invalid_argument("Transform hierarchy cycle");
            auto p = ids.find(t->parent);
            if (p == ids.end() || !p->second->transform)
                throw std::invalid_argument("Missing parent Transform");
            t = &*p->second->transform;
        }
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
