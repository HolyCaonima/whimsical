#include "Systems.h"
#include "Validation.h"
#include "core/CpuProfile.h"
namespace afterlight {
RenderSystem::RenderSystem(SceneStorage& storage, const std::vector<Material>& m) : s(storage), materials(m) {
    s.changes.subscribe<WorldPoseChanged>([this](Entity e) { publishTransform(e); });
    s.changes.subscribe<RenderTransformChanged>([this](Entity e) { publishTransform(e); });
    s.changes.subscribe<EffectiveEnabledChanged>([this](Entity e) { publishAttributes(e); });
    s.changes.subscribe<RenderAttributesChanged>([this](Entity e) { publishAttributes(e); });
    s.changes.subscribe<Interactable>([this](Entity e) { publishAttributes(e); });
    s.changes.subscribe<GeometryChanged>([this](Entity e) {
        if (auto r = s.registry.tryGet<Renderable>(e))
            s.renderScene.geometryChanged(r->slot);
    });
}
static ProxyTransform proxyTransform(const Transform& t, const Renderable& r) {
    return {t.world.position + t.world.rotation * r.appearance.offset,
            r.appearance.scale * r.appearance.animationScale, t.world.rotation};
}
void RenderSystem::add(Entity e, RenderComponent appearance, std::shared_ptr<const StaticMesh> mesh) {
    if (appearance.material >= materials.size())
        throw std::out_of_range("Invalid material");
    validateRenderAppearance(appearance);
    auto& t = s.registry.get<Transform>(e);
    auto batch = Changes::Batch(s.changes);
    auto& r = s.registry.emplace<Renderable>(e, Renderable{appearance, std::move(mesh)});
    r.slot = s.renderScene.create(proxyTransform(t, r),
                                  {e, appearance.material, appearance.shape,
                                   s.enabled(e) && appearance.visible, s.registry.has<Interactable>(e)});
    if (r.mesh)
        s.changes.mark<GeometryChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::set(Entity e, RenderComponent appearance, std::shared_ptr<const StaticMesh> mesh) {
    validateRenderAppearance(appearance);
    if (appearance.material >= materials.size())
        throw std::invalid_argument("Invalid material");
    if (mesh && s.registry.has<Skin>(e))
        throw std::invalid_argument("Remove skin before static geometry");
    auto batch = Changes::Batch(s.changes);
    setStaticMesh(e, std::move(mesh));
    s.registry.get<Renderable>(e).appearance = appearance;
    s.changes.mark<RenderTransformChanged>(e);
    s.changes.mark<RenderAttributesChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::remove(Entity e) {
    s.removeComponent(e, typeid(Renderable));
}
void RenderSystem::publishTransform(Entity e) {
    if (auto r = s.registry.tryGet<Renderable>(e))
        s.renderScene.setTransform(r->slot, proxyTransform(s.registry.get<Transform>(e), *r));
}
void RenderSystem::publishAttributes(Entity e) {
    if (auto r = s.registry.tryGet<Renderable>(e))
        s.renderScene.setAttributes(r->slot,
                                    {e, r->appearance.material, r->appearance.shape,
                                     s.enabled(e) && r->appearance.visible, s.registry.has<Interactable>(e)});
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
void RenderSystem::setVisualPose(Entity e, vec3 offset, vec3 scale) {
    auto batch = Changes::Batch(s.changes);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(offset[i]) || !std::isfinite(scale[i]) || scale[i] <= 0)
            throw std::invalid_argument("Invalid visual pose");
    auto& r = s.registry.get<Renderable>(e).appearance;
    r.offset = offset;
    r.animationScale = scale;
    s.changes.mark<RenderTransformChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::setMaterial(Entity e, uint32_t material) {
    auto batch = Changes::Batch(s.changes);
    if (material >= materials.size())
        throw std::out_of_range("Invalid material");
    s.registry.get<Renderable>(e).appearance.material = material;
    s.changes.mark<RenderAttributesChanged>(e);
    s.changes.mark<Renderable>(e);
    batch.commit();
}
void RenderSystem::setScale(Entity e, vec3 scale) {
    auto batch = Changes::Batch(s.changes);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(scale[i]) || scale[i] <= 0)
            throw std::invalid_argument("Invalid render scale");
    s.registry.get<Renderable>(e).appearance.scale = scale;
    s.changes.mark<RenderTransformChanged>(e);
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
void RenderSystem::extract(Frame& f, bool debug) {
    s.changes.requireCommitted();
    for (auto e : s.registry.view<Transform, PointLight>()) {
        if (!s.enabled(e))
            continue;
        const auto& light = s.registry.get<PointLight>(e);
        f.lightEntities.push_back(e);
        f.lights.push_back({vec4(s.registry.get<Transform>(e).world.position, light.radius),
                            vec4(light.color, light.intensity)});
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
            auto position = t.position + t.rotation * p.position;
            pose.jointWorld.push_back(glm::translate(mat4(1), position) *
                                      glm::mat4_cast(t.rotation * p.rotation));
        }
        // Disabled animation retains its final pose and geometry binding. Visibility
        // is an instance flag, never a mesh resource lifetime event.
        if (auto skin = s.registry.tryGet<Skin>(e)) {
            Frame::Skin draw;
            draw.slot = s.registry.get<Renderable>(e).slot;
            draw.mesh = skin->mesh;
            const auto& appearance = s.registry.get<Renderable>(e).appearance;
            auto root = glm::translate(mat4(1), t.position) * glm::mat4_cast(t.rotation);
            auto visual = root * glm::translate(mat4(1), appearance.offset) *
                          glm::scale(mat4(1), appearance.scale * appearance.animationScale) *
                          glm::inverse(root);
            for (size_t j = 0; j < skin->joints.size(); ++j)
                draw.palette.push_back(visual * pose.jointWorld[skin->joints[j]] *
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
} // namespace afterlight
