#include "Systems.h"
#include "Validation.h"
#include "core/CpuProfile.h"
#include <algorithm>
namespace afterlight {
static void publishJoints(SceneStorage& s, Entity e, std::vector<PhysicsPose> joints) {
    s.registry.get<Transform>(e);
    for (const auto& p : joints)
        validateRigidPose(p);
    for (const auto& binding : s.jointColliders.describe(e))
        if (binding.joint >= joints.size())
            throw std::invalid_argument("Joint layout still has collider bindings");
    if (auto j = s.registry.tryGet<JointPose>(e))
        j->model = std::move(joints);
    else
        s.registry.emplace<JointPose>(e, JointPose{std::move(joints)});
    s.changes.mark<JointPoseChanged>(e);
}
static std::vector<PhysicsPose> modelPose(const Animator& a) {
    auto model = a.solver().skeleton().toModel(a.solver().output().localPose);
    std::vector<PhysicsPose> joints;
    joints.reserve(model.size());
    for (const auto& p : model)
        joints.push_back({p.position + a.rootOffset, p.rotation});
    return joints;
}
AnimationSystem::AnimationSystem(SceneStorage& storage, MotionSystem& m) : s(storage), motion(m) {
    auto sync = [this](Entity e) {
        if (auto j = s.registry.tryGet<JointPose>(e))
            s.jointColliders.update(e, s.registry.get<Transform>(e).world, j->model);
    };
    s.changes.subscribe<WorldPoseChanged>(sync);
    s.changes.subscribe<JointPoseChanged>(sync);
    s.changes.subscribe<EffectiveEnabledChanged>(
        [this](Entity e) { s.jointColliders.setEnabled(e, s.enabled(e)); });
    s.changes.subscribeEvent<CharacterResized>([this](const CharacterResized& change) {
        auto binding = s.registry.tryGet<RootMotionBinding>(change.entity);
        if (!binding || !binding->preserveAnchor)
            return;
        auto& a = s.registry.get<Animator>(change.entity);
        a.rootOffset.y -= change.centerDelta;
        for (auto& joint : s.registry.get<JointPose>(change.entity).model)
            joint.position.y -= change.centerDelta;
        s.changes.mark<JointPoseChanged>(change.entity);
    });
}
void AnimationSystem::attachAnimation(Entity e, std::shared_ptr<const animation::Skeleton> skeleton,
                                      std::unique_ptr<animation::Solver> solver, vec3 offset) {
    attachAnimationInstance(e, std::make_unique<animation::Instance>(std::move(skeleton), std::move(solver)),
                            offset);
}
void AnimationSystem::attachAnimation(Entity e, const animation::Asset& asset, vec3 offset,
                                      const animation::AttributeValues& attributes) {
    auto instance = std::make_unique<animation::Instance>(asset);
    for (const auto& v : attributes)
        instance->setAttribute(v.first, v.second);
    auto batch = Changes::Batch(s.changes);
    attachAnimationInstance(e, std::move(instance), offset);
    if (!asset.header().id.empty())
        s.registry.get<Animator>(e).asset = asset.reference();
    batch.commit();
}
void AnimationSystem::attachAnimationInstance(Entity e, std::unique_ptr<animation::Instance> instance,
                                              vec3 offset) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(offset[i]))
            throw std::invalid_argument("Invalid animation root offset");
    s.registry.get<Transform>(e);
    if (!s.registry.has<Animator>(e) && s.registry.has<JointPose>(e))
        throw std::logic_error("Remove authored joints before attaching Animator");
    if (auto j = s.registry.tryGet<JointPose>(e);
        j && !j->model.empty() && j->model.size() != instance->skeleton().size())
        throw std::invalid_argument("Detach joints before changing skeleton layout");
    auto batch = Changes::Batch(s.changes);
    if (auto a = s.registry.tryGet<Animator>(e)) {
        const auto& before = a->instance->skeleton().joints();
        const auto& after = instance->skeleton().joints();
        if (before.size() != after.size())
            throw std::invalid_argument("Detach animation before changing skeleton layout");
        for (size_t i = 0; i < before.size(); ++i)
            if (before[i].name != after[i].name || before[i].parent != after[i].parent)
                throw std::invalid_argument("Detach animation before changing skeleton layout");
        *a = Animator{std::move(instance), offset};
    } else
        s.registry.emplace<Animator>(e, Animator{std::move(instance), offset});
    publishJoints(s, e, modelPose(s.registry.get<Animator>(e)));
    s.registry.get<JointPose>(e).skeleton =
        std::make_shared<animation::Skeleton>(s.registry.get<Animator>(e).solver().skeleton());
    batch.commit();
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
    s.removeComponent(e, typeid(Skin));
}
void AnimationSystem::removeJoints(Entity e) {
    s.removeComponent(e, typeid(JointPose));
}
void AnimationSystem::removeJointColliders(Entity e) {
    s.removeComponent(e, typeid(JointColliders));
}
void AnimationSystem::detachAnimation(Entity e) {
    s.removeComponent(e, typeid(Animator));
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
    auto layout = s.registry.get<JointPose>(e).skeleton;
    if (!layout)
        throw std::invalid_argument("Skin requires a named joint layout");
    const auto& joints = layout->joints();
    std::vector<unsigned> mapping;
    for (const auto& binding : mesh->bindings) {
        auto it = std::find_if(joints.begin(), joints.end(),
                               [&](const animation::Joint& j) { return j.name == binding.joint; });
        if (it == joints.end())
            throw std::invalid_argument("Mesh binding absent from skeleton: " + binding.joint);
        mapping.push_back(unsigned(it - joints.begin()));
    }
    auto batch = Changes::Batch(s.changes);
    if (auto skin = s.registry.tryGet<Skin>(e))
        *skin = Skin{std::move(mesh), std::move(mapping)};
    else
        s.registry.emplace<Skin>(e, Skin{std::move(mesh), std::move(mapping)});
    s.changes.mark<GeometryChanged>(e);
    batch.commit();
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
void AnimationSystem::setAnimationJoints(Entity e, std::vector<PhysicsPose> joints,
                                         std::shared_ptr<const animation::Skeleton> layout) {
    if (s.registry.has<Animator>(e))
        throw std::logic_error("Solved joints are owned by Animator");
    if (!layout)
        if (auto current = s.registry.tryGet<JointPose>(e))
            layout = current->skeleton;
    if (layout && layout->size() != joints.size())
        throw std::invalid_argument("Joint layout size mismatch");
    if (auto current = s.registry.tryGet<JointPose>(e);
        current && s.registry.has<Skin>(e) && layout != current->skeleton)
        throw std::logic_error("Remove skin before changing joint layout");
    auto batch = Changes::Batch(s.changes);
    publishJoints(s, e, std::move(joints));
    s.registry.get<JointPose>(e).skeleton = std::move(layout);
    batch.commit();
}
BodyHandle AnimationSystem::addAnimationCollider(Entity e, uint32_t joint, const ColliderShape& shape,
                                                 PhysicsPose local, bool blocking) {
    auto& joints = s.registry.get<JointPose>(e).model;
    if (joint >= joints.size())
        throw std::out_of_range("Publish joints before binding collider");
    auto batch = Changes::Batch(s.changes);
    auto h = s.jointColliders.bind(e, joint, shape, local, blocking);
    if (!s.registry.has<JointColliders>(e))
        s.registry.emplace<JointColliders>(e);
    s.jointColliders.update(e, s.registry.get<Transform>(e).world, joints);
    s.jointColliders.setEnabled(e, s.enabled(e));
    batch.commit();
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
        motion.consumeRootMotion(e, output.rootMotion, a.rootOffset);
        publishJoints(s, e, modelPose(a));
    }
}
} // namespace afterlight
