#include "ComponentCatalog.h"
#include "DataComponent.h"
#include "Validation.h"
#include "core/World.h"
#include "scene/SceneAsset.h"
#include "assets/EngineAssets.h"
#include <algorithm>

namespace afterlight {
namespace {
Json vector(vec3 v) {
    return Json::array({v.x, v.y, v.z});
}
vec3 vector3(const Json& j) {
    if (j.elements().size() != 3)
        throw std::invalid_argument("Expected vec3");
    return {j.at(0).number(), j.at(1).number(), j.at(2).number()};
}
Json pose(const PhysicsPose& p) {
    return {{"position", vector(p.position)},
            {"rotation", Json::array({p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w})}};
}
PhysicsPose pose(const Json& j) {
    PhysicsPose p;
    if (j.contains("position"))
        p.position = vector3(j.at("position"));
    if (j.contains("rotation")) {
        const auto& q = j.at("rotation");
        if (q.elements().size() != 4)
            throw std::invalid_argument("Expected quaternion");
        p.rotation = quat(float(q.at(3).number()), float(q.at(0).number()), float(q.at(1).number()),
                          float(q.at(2).number()));
    }
    validateRigidPose(p);
    return p;
}
Json shape(const ColliderShape& s) {
    if (s.type == ColliderType::Box)
        return {{"type", "box"}, {"halfExtents", vector(s.halfExtents)}};
    return {{"type", "capsule"}, {"radius", s.radius}, {"height", s.height()}};
}
ColliderShape shape(const Json& j) {
    if (j.at("type").string() == "box")
        return ColliderShape::box(vector3(j.at("halfExtents")));
    if (j.at("type").string() == "capsule")
        return ColliderShape::capsule(float(j.at("radius").number()), float(j.at("height").number()));
    throw std::invalid_argument("Unknown collider shape");
}
template <class D, class Decode, class Encode>
void codec(ComponentContract& c, Decode decode, Encode encode) {
    c.decode = [decode](const Json& j) -> std::any { return decode(j); };
    c.encode = [encode](const std::any& value) { return encode(std::any_cast<const D&>(value)); };
}
template <class D, class Prepare, class Capture>
void binding(ComponentContract& c, Prepare prepare, Capture capture) {
    c.prepare = [prepare](const World& w, Entity e, const std::any& v, AssetManager& a) -> PreparedComponent {
        return prepare(w, e, std::any_cast<const D&>(v), a);
    };
    c.inspect = [capture](const World& w, Entity e) -> std::optional<std::any> {
        std::optional<D> value = capture(w, e);
        return value ? std::optional<std::any>(std::move(*value)) : std::nullopt;
    };
}
const char* modeName(RootMotionBinding::Mode mode) {
    switch (mode) {
    case RootMotionBinding::Mode::Transform:
        return "transform";
    case RootMotionBinding::Mode::Kinematic:
        return "kinematic";
    case RootMotionBinding::Mode::Grounded:
        return "grounded";
    }
    throw std::invalid_argument("Unknown root motion mode");
}
std::vector<ComponentDependency> rootDependencies(const RootMotionBinding& b) {
    if (b.mode == RootMotionBinding::Mode::Transform)
        return {};
    return {{"collider"}};
}
} // namespace
void registerBuiltinComponents(ComponentCatalog& catalog) {
    const auto cascade = OnDependencyRemoval::Cascade;
    {
        auto c = component<DrawEntityID, SceneDrawEntityID>("drawEntityID");
        codec<SceneDrawEntityID>(c,
            [](const Json& j) { return SceneDrawEntityID{AssetRef::fromJson(j.at("target"))}; },
            [](const SceneDrawEntityID& v) { return Json{{"target", v.target.json()}}; });
        binding<SceneDrawEntityID>(c,
            [](const World&, Entity e, const SceneDrawEntityID& v, AssetManager& assets) {
                auto rt = assets.load<RenderTargetAsset>(v.target);
                if (rt->format != PixelFormat::R32Uint)
                    throw std::invalid_argument("DrawEntityID requires an R32Uint RenderTarget");
                return [e, rt](ComponentAccess& access) {
                    auto& r = access.storage.registry;
                    if (r.has<DrawEntityID>(e))
                        r.get<DrawEntityID>(e).target = rt;
                    else
                        r.emplace<DrawEntityID>(e, DrawEntityID{rt});
                };
            },
            [](const World& w, Entity e) {
                return SceneDrawEntityID{w.get<DrawEntityID>(e).target->reference()};
            });
        c.resolveReferences = [](std::any& value, const AssetManager& assets) {
            auto& v = std::any_cast<SceneDrawEntityID&>(value);
            v.target = assets.resolve(v.target);
        };
        c.erase = [](ComponentAccess& access, Entity e) { access.storage.registry.remove<DrawEntityID>(e); };
        catalog.add(std::move(c));
    }
    {
        auto c = component<Transform, SceneTransform>("transform");
        c.validateValue = [](const std::any& value) {
            validateRigidPose(std::any_cast<const SceneTransform&>(value).local);
        };
        codec<SceneTransform>(
            c,
            [](const Json& j) {
                return SceneTransform{pose(j), j.contains("parent") ? j.at("parent").string() : ""};
            },
            [](const SceneTransform& v) {
                auto j = pose(v.local);
                j["parent"] = v.parent;
                return j;
            });
        binding<SceneTransform>(
            c,
            [](const World& w, Entity e, const SceneTransform& v, AssetManager&) {
                Entity parent = v.parent.empty() ? 0 : w.findObject(v.parent);
                if (!v.parent.empty() && !parent)
                    throw std::invalid_argument("Missing parent entity");
                if (parent) {
                    w.get<Transform>(parent);
                    for (auto p = parent; p; p = w.get<Transform>(p).parent)
                        if (p == e)
                            throw std::invalid_argument("Transform hierarchy cycle");
                }
                return [e, v, parent](ComponentAccess& access) {
                    auto& w = access.world;
                    if (w.has<Transform>(e)) {
                        w.transforms.setParent(e, parent, false);
                        w.transforms.setLocal(e, v.local);
                    } else {
                        w.transforms.add(e, v.local);
                        if (parent)
                            w.transforms.setParent(e, parent, false);
                    }
                };
            },
            [](const World& w, Entity e) {
                const auto& t = w.get<Transform>(e);
                return SceneTransform{t.local, t.parent ? w.get<Identity>(t.parent).persistentId : ""};
            });
        c.erase = [](ComponentAccess& access, Entity e) {
            auto& w = access.world;
            auto children = w.get<Transform>(e).children;
            for (auto child : children)
                w.transforms.setParent(child, 0);
            w.transforms.setParent(e, 0);
            access.storage.registry.remove<Transform>(e);
        };
        catalog.add(std::move(c));
    }
    {
        auto c = component<Renderable, SceneRender>("render");
        c.dependencies = {{"transform"}};
        c.validateValue = [](const std::any& value) {
            const auto& render = std::any_cast<const SceneRender&>(value);
            validateRenderAppearance(render.appearance);
            validatePersistentId(render.material.id);
        };
        codec<SceneRender>(
            c,
            [](const Json& j) {
                SceneRender v;
                auto& a = v.appearance;
                if (j.contains("shape"))
                    throw std::invalid_argument("Render.shape was removed; bind a Mesh asset instead");
                if (j.contains("scale"))
                    a.scale = vector3(j.at("scale"));
                if (j.contains("offset"))
                    a.offset = vector3(j.at("offset"));
                if (j.contains("animationScale"))
                    a.animationScale = vector3(j.at("animationScale"));
                v.material = AssetRef::fromJson(j.at("material"));
                if (j.contains("visible"))
                    a.visible = j.at("visible").boolean();
                if (j.contains("castShadow"))
                    a.castShadow = j.at("castShadow").boolean();
                if (j.contains("overlay"))
                    a.overlay = j.at("overlay").boolean();
                if (j.contains("overlayColor"))
                    a.overlayColor = vector3(j.at("overlayColor"));
                if (j.contains("mesh"))
                    v.mesh = AssetRef::fromJson(j.at("mesh"));
                validateRenderAppearance(a);
                return v;
            },
            [](const SceneRender& v) {
                const auto& a = v.appearance;
                Json j{{"scale", vector(a.scale)},
                       {"offset", vector(a.offset)},
                       {"animationScale", vector(a.animationScale)},
                       {"material", v.material.json()},
                       {"visible", a.visible}, {"castShadow", a.castShadow}};
                if (a.overlay)
                    j["overlay"] = true;
                if (a.overlay || a.overlayColor != vec3(1))
                    j["overlayColor"] = vector(a.overlayColor);
                if (v.mesh)
                    j["mesh"] = v.mesh->json();
                return j;
            });
        binding<SceneRender>(
            c,
            [](const World&, Entity e, const SceneRender& v, AssetManager& a) {
                auto appearance = v.appearance;
                appearance.material = a.load<MaterialAsset>(v.material);
                auto mesh = v.mesh ? a.load<StaticMesh>(*v.mesh) : nullptr;
                return [e, appearance, mesh](ComponentAccess& access) {
                    auto& w = access.world;
                    if (w.has<Renderable>(e))
                        w.render.set(e, appearance, mesh);
                    else
                        w.render.add(e, appearance, mesh);
                };
            },
            [](const World& w, Entity e) {
                const auto& r = w.get<Renderable>(e);
                SceneRender v{r.appearance};
                v.material = r.appearance.material->reference();
                v.appearance.material.reset();
                if (r.mesh)
                    v.mesh = r.mesh->reference();
                return v;
            });
        c.resolveReferences = [](std::any& value, const AssetManager& assets) {
            auto& v = std::any_cast<SceneRender&>(value);
            v.material = assets.resolve(v.material);
            if (v.mesh)
                v.mesh = assets.resolve(*v.mesh);
        };
        c.erase = [](ComponentAccess& access, Entity e) {
            auto& w = access.world;
            auto& s = access.storage;
            const auto& r = w.get<Renderable>(e);
            if (r.mesh)
                s.renderScene.geometryChanged(r.slot);
            s.renderScene.destroy(r.slot);
            s.registry.remove<Renderable>(e);
        };
        catalog.add(std::move(c));
    }
    {
        auto c = component<Collider, SceneCollider>("collider");
        c.dependencies = {{"transform"}};
        c.validateValue = [](const std::any& value) {
            validateColliderShape(std::any_cast<const SceneCollider&>(value).shape);
        };
        codec<SceneCollider>(
            c,
            [](const Json& j) {
                SceneCollider v;
                v.shape = shape(j.at("shape"));
                if (j.contains("motion")) {
                    auto m = j.at("motion").string();
                    if (m != "static" && m != "kinematic")
                        throw std::invalid_argument("Unknown body motion");
                    v.motion = m == "static" ? BodyMotion::Static : BodyMotion::Kinematic;
                }
                if (j.contains("layer"))
                    v.layer = j.at("layer").uint();
                if (j.contains("blocking"))
                    v.blocking = j.at("blocking").boolean();
                if (j.contains("walkable"))
                    v.walkable = j.at("walkable").boolean();
                if (j.contains("pickable"))
                    v.pickable = j.at("pickable").boolean();
                return v;
            },
            [](const SceneCollider& v) {
                return Json{{"shape", shape(v.shape)},
                            {"motion", v.motion == BodyMotion::Static ? "static" : "kinematic"},
                            {"layer", v.layer},
                            {"blocking", v.blocking},
                            {"walkable", v.walkable},
                            {"pickable", v.pickable}};
            });
        binding<SceneCollider>(
            c,
            [](const World&, Entity e, const SceneCollider& v, AssetManager&) {
                PhysicsBody b;
                b.shape = v.shape;
                b.motion = v.motion;
                b.layer = v.layer;
                b.blocking = v.blocking;
                b.walkable = v.walkable;
                b.pickable = v.pickable;
                return [e, b](ComponentAccess& access) {
                    auto& w = access.world;
                    if (w.has<Collider>(e))
                        w.motion.set(e, b);
                    else
                        w.motion.add(e, b);
                };
            },
            [](const World& w, Entity e) {
                const auto& b = w.motion.collider(e);
                return SceneCollider{b.shape, b.motion, b.layer, b.blocking, b.walkable, b.pickable};
            });
        c.erase = [](ComponentAccess& access, Entity e) {
            access.storage.physics.destroy(access.world.get<Collider>(e).body);
            access.storage.registry.remove<Collider>(e);
        };
        catalog.add(std::move(c));
    }
    {
        auto c = component<Animator, SceneAnimation>("animator");
        c.dependencies = {{"transform"}};
        c.owns = {"joints"};
        c.validateValue = [](const std::any& value) {
            if (!std::isfinite(glm::length(std::any_cast<const SceneAnimation&>(value).rootOffset)))
                throw std::invalid_argument("Invalid root offset");
        };
        codec<SceneAnimation>(
            c,
            [](const Json& j) {
                if (j.contains("rootMotion"))
                    throw std::invalid_argument("Use the rootMotion component");
                SceneAnimation v;
                v.asset = AssetRef::fromJson(j.at("asset"));
                if (j.contains("rootOffset"))
                    v.rootOffset = vector3(j.at("rootOffset"));
                if (j.contains("attributes"))
                    for (const auto& [key, value] : j.at("attributes").members())
                        v.attributes[key] = value.string();
                return v;
            },
            [](const SceneAnimation& v) {
                Json attributes = Json::object();
                for (const auto& [key, value] : v.attributes)
                    attributes[key] = value;
                return Json{{"asset", v.asset.json()},
                            {"rootOffset", vector(v.rootOffset)},
                            {"attributes", attributes}};
            });
        binding<SceneAnimation>(
            c,
            [](const World& w, Entity e, const SceneAnimation& v, AssetManager& a) -> PreparedComponent {
                auto asset = a.load<animation::Asset>(v.asset);
                if (auto current = w.registry().tryGet<Animator>(e);
                    current && current->asset && current->asset->id == asset->header().id) {
                    return [e, v](ComponentAccess& access) {
                        access.world.animation.configureAnimation(e, v.rootOffset, v.attributes);
                    };
                }
                auto instance = std::make_shared<std::unique_ptr<animation::Instance>>(
                    std::make_unique<animation::Instance>(*asset));
                for (const auto& [key, value] : v.attributes)
                    (*instance)->setAttribute(key, value);
                if (!std::isfinite(glm::length(v.rootOffset)))
                    throw std::invalid_argument("Invalid root offset");
                return [e, v, asset, instance](ComponentAccess& access) {
                    auto& w = access.world;
                    // The costly solver was prepared before the mutation boundary.
                    w.animation.attachPrepared(e, std::move(*instance), v.rootOffset, asset->reference());
                };
            },
            [](const World& w, Entity e) {
                const auto& v = w.get<Animator>(e);
                return SceneAnimation{v.asset.value_or(AssetRef{}), v.rootOffset, v.solver().attributes()};
            });
        c.resolveReferences = [](std::any& value, const AssetManager& assets) {
            auto& v = std::any_cast<SceneAnimation&>(value);
            if (v.asset.id.empty())
                throw std::invalid_argument("Cannot save an unregistered animation solver");
            v.asset = assets.resolve(v.asset);
        };
        c.erase = [](ComponentAccess& access, Entity e) { access.storage.registry.remove<Animator>(e); };
        catalog.add(std::move(c));
    }
    {
        auto c = component<RootMotionBinding, RootMotionBinding>("rootMotion");
        c.dependencies = {{"animator", cascade}};
        codec<RootMotionBinding>(
            c,
            [](const Json& j) {
                RootMotionBinding v;
                if (j.contains("mode")) {
                    auto mode = j.at("mode").string();
                    if (mode == "kinematic")
                        v.mode = RootMotionBinding::Mode::Kinematic;
                    else if (mode == "grounded")
                        v.mode = RootMotionBinding::Mode::Grounded;
                    else if (mode != "transform")
                        throw std::invalid_argument("Unknown root motion mode");
                }
                if (j.contains("mask"))
                    v.mask = j.at("mask").uint();
                if (j.contains("preserveAnchor"))
                    v.preserveAnchor = j.at("preserveAnchor").boolean();
                return v;
            },
            [](const RootMotionBinding& v) {
                return Json{
                    {"mode", modeName(v.mode)}, {"mask", v.mask}, {"preserveAnchor", v.preserveAnchor}};
            });
        c.extraDependencies = [](const std::any& v) {
            return rootDependencies(std::any_cast<const RootMotionBinding&>(v));
        };
        c.runtimeDependencies = [](const World& w, Entity e) {
            return rootDependencies(w.get<RootMotionBinding>(e));
        };
        c.validate = [](const ComponentSet& set, const std::any& value) {
            if (std::any_cast<const RootMotionBinding&>(value).mode == RootMotionBinding::Mode::Grounded)
                if (auto collider = set.find<SceneCollider>();
                    collider && collider->shape.type != ColliderType::Capsule)
                    throw std::invalid_argument("Grounded root motion requires capsule");
        };
        binding<RootMotionBinding>(
            c,
            [](const World&, Entity e, const RootMotionBinding& v, AssetManager&) {
                return [e, v](ComponentAccess& access) {
                    auto& w = access.world;
                    w.motion.bindRootMotion(e, v);
                };
            },
            [](const World& w, Entity e) { return w.get<RootMotionBinding>(e); });
        c.erase = [](ComponentAccess& access, Entity e) {
            access.storage.registry.remove<RootMotionBinding>(e);
        };
        catalog.add(std::move(c));
    }
    {
        auto c = component<JointPose, SceneJoints>("joints");
        c.dependencies = {{"transform"}};
        c.validateValue = [](const std::any& value) {
            const auto& joints = std::any_cast<const SceneJoints&>(value);
            for (const auto& p : joints.poses)
                validateRigidPose(p);
            if (!joints.layout.empty() && joints.layout.size() != joints.poses.size())
                throw std::invalid_argument("Joint layout size mismatch");
        };
        codec<SceneJoints>(
            c,
            [](const Json& j) {
                SceneJoints v;
                const auto& poses = std::holds_alternative<Json::Array>(j.value()) ? j : j.at("poses");
                for (const auto& p : poses.elements())
                    v.poses.push_back(pose(p));
                if (std::holds_alternative<Json::Object>(j.value()) && j.contains("layout"))
                    for (const auto& joint : j.at("layout").elements())
                        v.layout.push_back({joint.at("name").string(), int(joint.at("parent").number()), {}});
                if (!v.layout.empty() && v.layout.size() != v.poses.size())
                    throw std::invalid_argument("Joint layout size mismatch");
                return v;
            },
            [](const SceneJoints& v) {
                Json poses = Json::array();
                for (const auto& p : v.poses)
                    poses.push(pose(p));
                if (v.layout.empty())
                    return poses;
                Json layout = Json::array();
                for (const auto& joint : v.layout)
                    layout.push({{"name", joint.name}, {"parent", joint.parent}});
                return Json{{"poses", poses}, {"layout", layout}};
            });
        binding<SceneJoints>(
            c,
            [](const World&, Entity e, const SceneJoints& v, AssetManager&) {
                std::shared_ptr<const animation::Skeleton> layout;
                if (!v.layout.empty())
                    layout = std::make_shared<animation::Skeleton>(v.layout);
                return [e, v, layout](ComponentAccess& access) {
                    auto& w = access.world;
                    w.animation.setAnimationJoints(e, v.poses, layout);
                };
            },
            [](const World& w, Entity e) -> std::optional<SceneJoints> {
                if (w.has<Animator>(e))
                    return std::nullopt;
                const auto& j = w.get<JointPose>(e);
                return SceneJoints{j.model,
                                   j.skeleton ? j.skeleton->joints() : std::vector<animation::Joint>{}};
            });
        c.erase = [](ComponentAccess& access, Entity e) { access.storage.registry.remove<JointPose>(e); };
        catalog.add(std::move(c));
    }
    {
        auto c = component<Skin, AssetRef>("skin");
        c.dependencies = {{"render", cascade}, {"joints", cascade}};
        codec<AssetRef>(
            c, [](const Json& j) { return AssetRef::fromJson(j); },
            [](const AssetRef& v) { return v.json(); });
        c.validate = [](const ComponentSet& set, const std::any&) {
            if (auto r = set.find<SceneRender>(); r && r->mesh)
                throw std::invalid_argument("Skin and static geometry share one render binding");
        };
        binding<AssetRef>(
            c,
            [](const World&, Entity e, const AssetRef& v, AssetManager& a) {
                auto mesh = a.load<SkinnedMesh>(v);
                return [e, mesh](ComponentAccess& access) {
                    auto& w = access.world;
                    w.animation.setSkinnedMesh(e, mesh);
                };
            },
            [](const World& w, Entity e) { return w.get<Skin>(e).mesh->reference(); });
        c.resolveReferences = [](std::any& value, const AssetManager& assets) {
            auto& ref = std::any_cast<AssetRef&>(value);
            ref = assets.resolve(ref);
        };
        c.erase = [](ComponentAccess& access, Entity e) {
            auto& w = access.world;
            access.storage.renderScene.geometryChanged(w.get<Renderable>(e).slot);
            access.storage.registry.remove<Skin>(e);
        };
        catalog.add(std::move(c));
    }
    {
        auto c = component<JointColliders, SceneJointColliders>("jointColliders");
        c.dependencies = {{"joints", cascade}};
        c.validateValue = [](const std::any& value) {
            for (const auto& b : std::any_cast<const SceneJointColliders&>(value).bindings) {
                validateColliderShape(b.shape);
                validateRigidPose(b.local);
            }
        };
        codec<SceneJointColliders>(
            c,
            [](const Json& j) {
                SceneJointColliders v;
                for (const auto& b : j.elements())
                    v.bindings.push_back({b.at("joint").uint(), shape(b.at("shape")), pose(b.at("local")),
                                          b.at("blocking").boolean()});
                return v;
            },
            [](const SceneJointColliders& v) {
                Json j = Json::array();
                for (const auto& b : v.bindings)
                    j.push({{"joint", b.joint},
                            {"shape", shape(b.shape)},
                            {"local", pose(b.local)},
                            {"blocking", b.blocking}});
                return j;
            });
        binding<SceneJointColliders>(
            c,
            [](const World&, Entity e, const SceneJointColliders& v, AssetManager&) {
                return [e, v](ComponentAccess& access) {
                    auto& w = access.world;
                    w.animation.setAnimationColliders(e, v.bindings);
                };
            },
            [](const World& w, Entity e) { return SceneJointColliders{w.animation.describeColliders(e)}; });
        c.erase = [](ComponentAccess& access, Entity e) {
            access.storage.jointColliders.remove(e);
            access.storage.registry.remove<JointColliders>(e);
        };
        catalog.add(std::move(c));
    }
    {
        auto c = dataComponent<LightComponent>(
            "light",
            [](const Json& j) {
                LightComponent v;
                if (j.contains("type"))
                    v.type = parseLightType(j.at("type").string());
                if (j.contains("color"))
                    v.color = vector3(j.at("color"));
                if (j.contains("intensity"))
                    v.intensity = float(j.at("intensity").number());
                if (j.contains("radius"))
                    v.radius = float(j.at("radius").number());
                if (j.contains("width")) v.width = float(j.at("width").number());
                if (j.contains("height")) v.height = float(j.at("height").number());
                if (j.contains("length")) v.length = float(j.at("length").number());
                if (j.contains("innerAngle")) v.innerAngle = float(j.at("innerAngle").number());
                if (j.contains("outerAngle")) v.outerAngle = float(j.at("outerAngle").number());
                if (j.contains("angularRadius")) v.angularRadius = float(j.at("angularRadius").number());
                if (j.contains("twoSided")) v.twoSided = j.at("twoSided").boolean();
                return v;
            },
            [](const LightComponent& v) {
                return Json{{"type", lightTypeName(v.type)}, {"color", vector(v.color)},
                            {"intensity", v.intensity}, {"radius", v.radius},
                            {"width", v.width}, {"height", v.height}, {"length", v.length},
                            {"innerAngle", v.innerAngle}, {"outerAngle", v.outerAngle},
                            {"angularRadius", v.angularRadius}, {"twoSided", v.twoSided}};
            },
            validateLight);
        c.dependencies = {{"transform"}};
        catalog.add(std::move(c));
    }
    catalog.add(dataComponent<Interactable>(
        "interactable",
        [](const Json& j) {
            (void)j.members();
            return Interactable{};
        },
        [](const Interactable&) { return Json::object(); }, [](const Interactable&) {}));
    catalog.add(dataComponent<ScriptData>(
        "data", [](const Json& j) { return ScriptData{j}; }, [](const ScriptData& v) { return v.value; },
        [](const ScriptData& v) { (void)v.value.members(); }));
}
} // namespace afterlight
