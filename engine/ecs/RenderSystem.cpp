#include "Systems.h"
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
    RenderScene::validateInstances(appearance.instanceCount, appearance.instanceTransforms);
    if (!appearance.material)
        throw std::invalid_argument("Render requires a Material asset");
    auto& t = s.registry.get<Transform>(e);
    auto batch = Changes::Batch(s.changes);
    auto& r = s.registry.emplace<Renderable>(e, Renderable{appearance, std::move(mesh)});
    r.slot = s.renderScene.create(proxyTransform(t),
                                  {e, 0, s.enabled(e) && appearance.visible, appearance.castShadow},
                                  appearance.material);
    s.renderScene.setInstances(r.slot, appearance.instanceCount, appearance.instanceTransforms);
    if (r.mesh)
        s.changes.mark<GeometryChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::set(Entity e, RenderComponent appearance, std::shared_ptr<const StaticMesh> mesh) {
    RenderScene::validateInstances(appearance.instanceCount, appearance.instanceTransforms);
    if (!appearance.material)
        throw std::invalid_argument("Render requires a Material asset");
    if (mesh && s.registry.has<Skin>(e))
        throw std::invalid_argument("Remove skin before static geometry");
    auto& r = s.registry.get<Renderable>(e);
    if (r.instanceDriver != typeid(void) &&
        (r.appearance.instanceCount != appearance.instanceCount ||
         r.appearance.instanceTransforms != appearance.instanceTransforms))
        throw std::logic_error("Render instances are owned by another driver");
    auto batch = Changes::Batch(s.changes);
    setStaticMesh(e, std::move(mesh));
    s.renderScene.setInstances(r.slot, appearance.instanceCount, appearance.instanceTransforms);
    r.appearance = std::move(appearance);
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
                                    {e, 0, s.enabled(e) && r->appearance.visible, r->appearance.castShadow},
                                    r->appearance.material);
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
void RenderSystem::setInstances(Entity e, uint32_t count, std::vector<ProxyTransform> transforms) {
    setInstancesAs(e, count, std::move(transforms), typeid(void));
}
void RenderSystem::claimInstances(Entity e, std::type_index owner) {
    auto& r = s.registry.get<Renderable>(e);
    if (owner == typeid(void) || (r.instanceDriver != typeid(void) && r.instanceDriver != owner))
        throw std::logic_error("Render instances already have a driver");
    r.instanceDriver = owner;
}
void RenderSystem::releaseInstances(Entity e, std::type_index owner) {
    auto& r = s.registry.get<Renderable>(e);
    if (r.instanceDriver == owner)
        r.instanceDriver = typeid(void);
}
void RenderSystem::setDrivenInstances(Entity e, uint32_t count, std::vector<ProxyTransform> transforms,
                                      std::type_index owner) {
    setInstancesAs(e, count, std::move(transforms), owner);
}
void RenderSystem::setInstancesAs(Entity e, uint32_t count, std::vector<ProxyTransform> transforms,
                                 std::type_index writer) {
    auto& r = s.registry.get<Renderable>(e);
    if (r.instanceDriver != writer)
        throw std::logic_error("Render instances are owned by another driver");
    s.renderScene.setInstances(r.slot, count, transforms);
    auto batch = Changes::Batch(s.changes);
    r.appearance.instanceCount = count;
    r.appearance.instanceTransforms = std::move(transforms);
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
