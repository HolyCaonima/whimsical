#include "Systems.h"
#include "Validation.h"
#include "core/CpuProfile.h"
#include "assets/MaterialAsset.h"
namespace whimsical {
RenderSystem::RenderSystem(SceneStorage& storage) : s(storage) {
    s.changes.subscribe<WorldPoseChanged>([this](Entity e) { publishTransform(e); });
    s.changes.subscribe<EffectiveEnabledChanged>([this](Entity e) { publishAttributes(e); });
    s.changes.subscribe<RenderAttributesChanged>([this](Entity e) { publishAttributes(e); });
    s.changes.subscribe<GeometryChanged>([this](Entity e) {
        if (auto r = s.registry.tryGet<Renderable>(e))
            s.renderScene.geometryChanged(r->slot);
    });
}
static ProxyTransform proxyTransform(const Transform& t) {
    return {t.world.position, t.world.scale, t.world.rotation};
}
void RenderSystem::add(Entity e, RenderComponent appearance, std::shared_ptr<const StaticMesh> mesh) {
    if (!appearance.material)
        throw std::invalid_argument("Render requires a Material asset");
    validateRenderAppearance(appearance);
    auto& t = s.registry.get<Transform>(e);
    auto batch = Changes::Batch(s.changes);
    auto& r = s.registry.emplace<Renderable>(e, Renderable{appearance, std::move(mesh)});
    r.slot = s.renderScene.create(proxyTransform(t),
                                  {e, 0,
                                   s.enabled(e) && appearance.visible,
                                   appearance.castShadow, appearance.overlay, appearance.overlayColor}, appearance.material);
    if (r.mesh)
        s.changes.mark<GeometryChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::set(Entity e, RenderComponent appearance, std::shared_ptr<const StaticMesh> mesh) {
    validateRenderAppearance(appearance);
    if (!appearance.material)
        throw std::invalid_argument("Render requires a Material asset");
    if (mesh && s.registry.has<Skin>(e))
        throw std::invalid_argument("Remove skin before static geometry");
    auto batch = Changes::Batch(s.changes);
    setStaticMesh(e, std::move(mesh));
    s.registry.get<Renderable>(e).appearance = appearance;
    s.changes.mark<RenderAttributesChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::remove(Entity e) {
    s.removeComponent(e, typeid(Renderable));
}
void RenderSystem::publishTransform(Entity e) {
    if (auto r = s.registry.tryGet<Renderable>(e))
        s.renderScene.setTransform(r->slot, proxyTransform(s.registry.get<Transform>(e)));
}
void RenderSystem::publishAttributes(Entity e) {
    if (auto r = s.registry.tryGet<Renderable>(e))
        s.renderScene.setAttributes(r->slot,
                                    {e, 0,
                                     s.enabled(e) && r->appearance.visible,
                                     r->appearance.castShadow, r->appearance.overlay, r->appearance.overlayColor}, r->appearance.material);
}
void RenderSystem::setStaticMesh(Entity e, std::shared_ptr<const StaticMesh> mesh) {
    auto batch = Changes::Batch(s.changes);
    if (mesh && s.registry.has<Skin>(e))
        throw std::invalid_argument("Remove skin before binding static geometry");
    auto& r = s.registry.get<Renderable>(e);
    if (r.mesh == mesh)
        return;
    r.mesh = std::move(mesh);
    s.changes.mark<GeometryChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::setMaterial(Entity e, std::shared_ptr<const MaterialAsset> material) {
    auto batch = Changes::Batch(s.changes);
    if (!material)
        throw std::invalid_argument("Render requires a Material asset");
    s.registry.get<Renderable>(e).appearance.material = std::move(material);
    s.changes.mark<RenderAttributesChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::setVisible(Entity e, bool visible) {
    auto batch = Changes::Batch(s.changes);
    s.registry.get<Renderable>(e).appearance.visible = visible;
    s.changes.mark<RenderAttributesChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::extract(Frame& f, bool debug, RenderTargetAccess& targets) {
    s.changes.requireCommitted();
    for (auto e : s.registry.view<DrawEntityID>())
        if (s.enabled(e)) {
            auto rt = targets.acquire(s.registry.get<DrawEntityID>(e).target);
            if (std::find(f.entityIDOutputs.begin(), f.entityIDOutputs.end(), rt) == f.entityIDOutputs.end())
                f.entityIDOutputs.push_back(std::move(rt));
        }
    for (auto e : s.registry.view<Transform, LightComponent>()) {
        if (!s.enabled(e))
            continue;
        const auto& light = s.registry.get<LightComponent>(e);
        f.lightEntities.push_back(e);
        const auto& pose = s.registry.get<Transform>(e).world;
        f.lights.push_back(packLight(light, pose.position, pose.rotation));
    }
    for (auto e : s.registry.view<Renderable>()) {
        const auto& r = s.registry.get<Renderable>(e);
        if (r.mesh)
            f.staticMeshes.push_back({r.slot, r.mesh});
    }
    for (auto e : s.registry.view<Transform, JointPose>()) {
        const auto& t = s.registry.get<Transform>(e).world;
        const auto& joints = s.registry.get<JointPose>(e).model;
        Frame::SkeletonPose pose;
        pose.owner = e;
        for (const auto& p : joints) {
            pose.jointWorld.push_back(transformMatrix(composeTransform(t, {p.position, p.rotation})));
        }
        // Disabled animation retains its final pose and geometry binding. Visibility
        // is an instance flag, never a mesh resource lifetime event.
        if (auto skin = s.registry.tryGet<Skin>(e)) {
            Frame::Skin draw;
            draw.slot = s.registry.get<Renderable>(e).slot;
            draw.mesh = skin->mesh;
            for (size_t j = 0; j < skin->joints.size(); ++j)
                draw.palette.push_back(pose.jointWorld[skin->joints[j]] *
                                       skin->mesh->bindings[j].inverseBind);
            f.skins.push_back(std::move(draw));
        }
        auto layout = s.registry.get<JointPose>(e).skeleton;
        if (debug && s.enabled(e) && layout)
            for (size_t j = 0; j < joints.size(); ++j) {
                int parent = layout->joints()[j].parent;
                if (parent >= 0)
                    f.physicsLines.push_back(
                        {vec3(pose.jointWorld[parent][3]), vec3(pose.jointWorld[j][3]), {.3f, 1, .5f}});
            }
        f.skeletons.push_back(std::move(pose));
    }
}
} // namespace whimsical
