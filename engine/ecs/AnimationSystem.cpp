#include "Systems.h"
#include "core/CpuProfile.h"
#include <algorithm>
namespace afterlight {
static void publishJoints(SceneStorage& s, Entity e, std::vector<PhysicsPose> joints) {
    s.jointColliders.update(e, s.registry.get<Transform>(e).world, joints);
    if (auto j = s.registry.tryGet<JointPose>(e))
        j->model = std::move(joints);
    else
        s.registry.emplace<JointPose>(e, JointPose{std::move(joints)});
}
static std::vector<PhysicsPose> modelPose(const Animator& a) {
    auto model = a.solver().skeleton().toModel(a.solver().output().localPose);
    std::vector<PhysicsPose> joints;
    joints.reserve(model.size());
    for (const auto& p : model)
        joints.push_back({p.position + a.rootOffset, p.rotation});
    return joints;
}
void AnimationSystem::attachAnimation(Entity e, std::shared_ptr<const animation::Skeleton> skeleton,
                                      std::unique_ptr<animation::Solver> solver, bool root, vec3 offset) {
    attachAnimationInstance(e, std::make_unique<animation::Instance>(std::move(skeleton), std::move(solver)),
                            root, offset);
}
void AnimationSystem::attachAnimation(Entity e, const animation::Asset& asset, bool root, vec3 offset,
                                      const animation::AttributeValues& attributes) {
    auto instance = std::make_unique<animation::Instance>(asset);
    for (const auto& v : attributes)
        instance->setAttribute(v.first, v.second);
    attachAnimationInstance(e, std::move(instance), root, offset);
    if (!asset.header().id.empty())
        s.registry.get<Animator>(e).asset = asset.reference();
}
void AnimationSystem::attachAnimationInstance(Entity e, std::unique_ptr<animation::Instance> instance,
                                              bool root, vec3 offset) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(offset[i]))
            throw std::invalid_argument("Invalid animation root offset");
    s.registry.get<Transform>(e);
    if (root)
        (void)motion.agent(e);
    if (auto j = s.registry.tryGet<JointPose>(e);
        j && !j->model.empty() && j->model.size() != instance->skeleton().size())
        throw std::invalid_argument("Detach joints before changing skeleton layout");
    if (auto a = s.registry.tryGet<Animator>(e)) {
        const auto& before = a->instance->skeleton().joints();
        const auto& after = instance->skeleton().joints();
        if (before.size() != after.size())
            throw std::invalid_argument("Detach animation before changing skeleton layout");
        for (size_t i = 0; i < before.size(); ++i)
            if (before[i].name != after[i].name || before[i].parent != after[i].parent)
                throw std::invalid_argument("Detach animation before changing skeleton layout");
        *a = Animator{std::move(instance), root, offset};
    } else
        s.registry.emplace<Animator>(e, Animator{std::move(instance), root, offset});
    publishJoints(s, e, modelPose(s.registry.get<Animator>(e)));
}
void AnimationSystem::setAnimationAttribute(Entity e, const std::string& key, const std::string& value) {
    s.registry.get<Animator>(e).instance->setAttribute(key, value);
}
AnimationInspection AnimationSystem::inspectAnimation(Entity e) const {
    auto a = s.registry.tryGet<Animator>(e);
    if (!a || !s.enabled(e))
        return {};
    auto& instance = *a->instance;
    return {e, s.registry.get<Identity>(e).name, instance.solverLabel(), instance.schema(),
            instance.attributes()};
}
void AnimationSystem::removeSkin(Entity e) {
    if (s.registry.has<Skin>(e))
        s.renderScene.geometryChanged(s.registry.get<Renderable>(e).slot);
    s.registry.remove<Skin>(e);
}
void AnimationSystem::removeJoints(Entity e) {
    if (s.registry.has<Animator>(e))
        throw std::logic_error("Detach animator before removing solved joints");
    removeJointColliders(e);
    s.registry.remove<JointPose>(e);
}
void AnimationSystem::removeJointColliders(Entity e) {
    s.registry.require(e);
    s.jointColliders.remove(e);
    s.registry.remove<JointColliders>(e);
}
void AnimationSystem::detachAnimation(Entity e) {
    s.registry.require(e);
    if (!s.registry.has<Animator>(e))
        return;
    removeSkin(e);
    s.registry.remove<Animator>(e);
    removeJoints(e);
}
void AnimationSystem::setSkinnedMesh(Entity e, std::shared_ptr<const SkinnedMesh> mesh) {
    if (!mesh) {
        removeSkin(e);
        return;
    }
    if (auto skin = s.registry.tryGet<Skin>(e); skin && skin->mesh == mesh)
        return;
    if (s.registry.get<Renderable>(e).mesh)
        throw std::invalid_argument("Remove static geometry before binding skin");
    const auto& joints = s.registry.get<Animator>(e).instance->skeleton().joints();
    std::vector<unsigned> mapping;
    for (const auto& binding : mesh->bindings) {
        auto it = std::find_if(joints.begin(), joints.end(),
                               [&](const animation::Joint& j) { return j.name == binding.joint; });
        if (it == joints.end())
            throw std::invalid_argument("Mesh binding absent from skeleton: " + binding.joint);
        mapping.push_back(unsigned(it - joints.begin()));
    }
    if (auto skin = s.registry.tryGet<Skin>(e))
        *skin = Skin{std::move(mesh), std::move(mapping)};
    else
        s.registry.emplace<Skin>(e, Skin{std::move(mesh), std::move(mapping)});
    s.renderScene.geometryChanged(s.registry.get<Renderable>(e).slot);
}
void AnimationSystem::setAnimationInput(Entity e, animation::Input input) {
    s.registry.get<Animator>(e).input = std::move(input);
}
void AnimationSystem::resetAnimation(Entity e) {
    auto& a = s.registry.get<Animator>(e);
    a.instance->reset();
    publishJoints(s, e, modelPose(a));
}
void AnimationSystem::setAnimationSolver(Entity e, std::unique_ptr<animation::Solver> solver) {
    auto& a = s.registry.get<Animator>(e);
    a.instance->setSolver(std::move(solver));
    a.asset.reset();
    publishJoints(s, e, modelPose(a));
}
const animation::Output& AnimationSystem::animationOutput(Entity e) const {
    return s.registry.get<Animator>(e).instance->output();
}
void AnimationSystem::setAnimationJoints(Entity e, std::vector<PhysicsPose> joints) {
    if (s.registry.has<Animator>(e))
        throw std::logic_error("Solved joints are owned by Animator");
    publishJoints(s, e, std::move(joints));
}
BodyHandle AnimationSystem::addAnimationCollider(Entity e, uint32_t joint, const ColliderShape& shape,
                                                 PhysicsPose local, bool blocking) {
    auto& joints = s.registry.get<JointPose>(e).model;
    if (joint >= joints.size())
        throw std::out_of_range("Publish joints before binding collider");
    auto h = s.jointColliders.bind(e, joint, shape, local, blocking);
    if (!s.registry.has<JointColliders>(e))
        s.registry.emplace<JointColliders>(e);
    s.jointColliders.update(e, s.registry.get<Transform>(e).world, joints);
    s.jointColliders.setEnabled(e, s.enabled(e));
    return h;
}
void AnimationSystem::update(float dt) {
    CpuScope scope("Animation Evaluation / Root Motion / Joint Colliders");
    auto entities = s.registry.view<Animator, Transform>();
    auto depth = [&](Entity e) {
        size_t n = 0;
        for (e = s.registry.get<Transform>(e).parent; e; e = s.registry.get<Transform>(e).parent)
            ++n;
        return n;
    };
    std::stable_sort(entities.begin(), entities.end(),
                     [&](Entity a, Entity b) { return depth(a) < depth(b); });
    for (auto e : entities) {
        if (!s.enabled(e))
            continue;
        auto& a = s.registry.get<Animator>(e);
        const auto& t = s.registry.get<Transform>(e).world;
        animation::Transform root{t.position + t.rotation * a.rootOffset, t.rotation};
        const auto& output = a.instance->evaluate(dt, root, a.input);
        if (a.applyRootMotion) {
            auto forward = output.rootMotion.rotation * vec3(0, 0, 1);
            vec3 delta =
                output.rootMotion.position + a.rootOffset - output.rootMotion.rotation * a.rootOffset;
            motion.rootMotion(e, delta, std::atan2(forward.x, forward.z));
        }
        publishJoints(s, e, modelPose(a));
    }
}
} // namespace afterlight
