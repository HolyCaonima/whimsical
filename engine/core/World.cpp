#include "World.h"
#include "CpuProfile.h"
#include "debug/PhysicsDebug.h"
#include <stdexcept>
#include <algorithm>
#include <cmath>
namespace afterlight {
ObjectPath World::objectPath(uint32_t id) const {
    return ObjectPath(mapAsset_.path, entity(id).persistentId);
}
uint32_t World::findObject(const std::string& id) const {
    auto found = objectIds_.find(id);
    return found == objectIds_.end() ? 0 : found->second;
}
uint32_t World::resolveObject(const ObjectPath& objectPath) const {
    return objectPath.map == mapAsset_.path ? findObject(objectPath.object) : 0;
}
void World::clearScene() {
    for (auto& o : objects_)
        if (o.alive)
            destroy(o.id);
    // Keep Entity tombstones and RenderScene/PhysicsScene allocators: stale runtime
    // identities cannot alias the next Map, and delta revisions remain monotonic.
    materials.clear();
    materialAssets.clear();
    textures.clear();
    lights.clear();
    animations_.clear();
    camera = {};
    navigation = {};
    sceneData = Json::object();
    sceneReferences.clear();
    sceneScripts.clear();
    mapAsset_ = {};
    playerId = selected = hovered = 0;
    path.clear();
    hasDestination = false;
    destination = vec3(0);
    state = "Idle";
    message.clear();
    resetHistory = true;
}
const GameObject& World::entity(uint32_t id) const {
    if (!id || id > objects_.size() || !objects_[id - 1].alive)
        throw std::out_of_range("Invalid object");
    return objects_[id - 1];
}
GameObject& World::mutableObject(uint32_t id) {
    return const_cast<GameObject&>(entity(id));
}
void World::setStaticMesh(uint32_t id, std::shared_ptr<const StaticMesh> mesh) {
    if (mesh && animations_.count(id))
        throw std::invalid_argument("StaticMesh cannot be bound to an animated object");
    mutableObject(id).staticMesh = std::move(mesh);
}
uint32_t World::spawn(std::string name, Shape shape, vec3 position, vec3 scale, uint32_t material,
                      bool blocking, bool interactable, std::string persistentId) {
    if (material >= materials.size())
        throw std::out_of_range("Invalid material");
    if (persistentId.empty())
        persistentId = newPersistentId();
    validatePersistentId(persistentId);
    if (objectIds_.count(persistentId))
        throw std::invalid_argument("Duplicate Object identity");
    GameObject o;
    o.persistentId = std::move(persistentId);
    o.id = uint32_t(objects_.size() + 1);
    o.name = std::move(name);
    o.position = position;
    o.render.shape = shape;
    o.render.scale = scale;
    o.render.material = material;
    o.interactable = interactable;
    PhysicsBody b;
    b.owner = o.id;
    b.pose.position = position;
    b.shape = shape == Shape::Box ? ColliderShape::box(scale * .5f)
                                  : ColliderShape::capsule(.4f * std::max(scale.x, scale.z), 2 * scale.y);
    b.layer = shape == Shape::Capsule ? CollisionLayer::Character : CollisionLayer::World;
    b.motion = shape == Shape::Capsule || interactable ? BodyMotion::Kinematic : BodyMotion::Static;
    b.blocking = blocking || shape == Shape::Capsule;
    b.pickable = blocking || interactable || shape == Shape::Capsule;
    o.physical = physics_.create(b);
    ProxyAttributes attributes;
    attributes.entity = o.id;
    attributes.material = material;
    attributes.shape = shape;
    attributes.interactable = interactable;
    o.proxy = scene_.create(proxyTransform(o), attributes);
    objects_.push_back(o);
    objectIds_.emplace(o.persistentId, o.id);
    return o.id;
}
ProxyTransform World::proxyTransform(const GameObject& o) {
    ProxyTransform t;
    t.position = o.position + o.rotation * o.render.offset;
    t.scale = o.render.scale * o.render.animationScale;
    t.rotation = o.rotation;
    return t;
}
void World::publishTransform(const GameObject& o) {
    scene_.setTransform(o.proxy, proxyTransform(o));
}
void World::publishAttributes(const GameObject& o) {
    ProxyAttributes a;
    a.entity = o.id;
    a.material = o.render.material;
    a.shape = o.render.shape;
    a.visible = o.alive && o.enabled && o.render.visible;
    a.interactable = o.interactable;
    scene_.setAttributes(o.proxy, a);
}
void World::syncPose(GameObject& o) {
    PhysicsPose pose{o.position, o.rotation};
    physics_.setPose(o.physical, pose);
    animationCollision_.update(o.id, pose, o.joints);
    publishTransform(o);
}
void World::setPose(uint32_t id, vec3 p, float yaw, float height) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(yaw) ||
        !std::isfinite(height) || height <= 0)
        throw std::invalid_argument("Invalid object pose");
    auto& o = mutableObject(id);
    o.position = p;
    o.rotation = glm::angleAxis(yaw, vec3(0, 1, 0));
    o.render.scale.y = height;
    physics_.setMotion(o.physical, BodyMotion::Kinematic);
    syncPose(o);
}
void World::setTransform(uint32_t id, const PhysicsPose& pose) {
    auto& o = mutableObject(id);
    // PhysicsScene validates before either physical or visual state is changed.
    physics_.setPose(o.physical, pose);
    physics_.setMotion(o.physical, BodyMotion::Kinematic);
    o.position = pose.position;
    o.rotation = pose.rotation;
    animationCollision_.update(id, pose, o.joints);
    publishTransform(o);
}
void World::setVisualPose(uint32_t id, vec3 offset, vec3 scale) {
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z) ||
        !std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
        glm::any(glm::lessThanEqual(scale, vec3(0))))
        throw std::invalid_argument("Invalid visual pose");
    auto& o = mutableObject(id);
    o.render.offset = offset;
    o.render.animationScale = scale;
    publishTransform(o);
}
void World::setMaterial(uint32_t id, uint32_t material) {
    if (material >= materials.size())
        throw std::out_of_range("Invalid material");
    auto& o = mutableObject(id);
    o.render.material = material;
    publishAttributes(o);
}
void World::configureCollider(uint32_t id, uint32_t layer, bool blocking, bool walkable, bool pickable) {
    physics_.setProperties(entity(id).physical, layer, blocking, walkable, pickable);
}
void World::setColliderShape(uint32_t id, const ColliderShape& shape) {
    physics_.setShape(entity(id).physical, shape);
}
void World::setSolid(uint32_t id, bool solid) {
    auto h = entity(id).physical;
    const auto b = physics_.body(h);
    physics_.setProperties(h, b.layer, solid, b.walkable, b.pickable);
}
void World::setEnabled(uint32_t id, bool enabled) {
    auto& o = mutableObject(id);
    o.enabled = enabled;
    physics_.setEnabled(o.physical, enabled);
    animationCollision_.setEnabled(id, enabled);
    publishAttributes(o);
}
void World::setVisible(uint32_t id, bool visible) {
    auto& o = mutableObject(id);
    o.render.visible = visible;
    publishAttributes(o);
}
void World::destroy(uint32_t id) {
    auto& o = mutableObject(id);
    animations_.erase(id);
    o.staticMesh.reset();
    objectIds_.erase(o.persistentId);
    for (auto ref = sceneReferences.begin(); ref != sceneReferences.end();) {
        if (ref->second == o.persistentId)
            ref = sceneReferences.erase(ref);
        else
            ++ref;
    }
    physics_.destroy(o.physical);
    animationCollision_.remove(id);
    o.alive = o.enabled = false;
    o.physical = {};
    scene_.destroy(o.proxy);
    if (selected == id)
        selected = 0;
    if (hovered == id)
        hovered = 0;
    if (playerId == id) {
        playerId = 0;
        path.clear();
        hasDestination = false;
    }
    resetHistory = true;
}
NavigationAgent World::agent(uint32_t id) const {
    const auto& b = physics_.body(entity(id).physical);
    if (b.shape.type != ColliderType::Capsule)
        throw std::invalid_argument("Movement requires a capsule body");
    if (glm::distance(b.pose.rotation * vec3(0, 1, 0), vec3(0, 1, 0)) > .001f)
        throw std::invalid_argument("Grounded movement requires an upright capsule; use moveBody for rigid motion");
    return {b.shape.radius, b.shape.height(), id};
}
vec3 World::feet(uint32_t id) const {
    auto a = agent(id);
    return physics_.body(entity(id).physical).pose.position - vec3(0, a.height * .5f, 0);
}
vec3 World::moveCharacter(uint32_t id, vec3 delta) {
    auto& o = mutableObject(id);
    auto a = agent(id);
    QueryFilter filter;
    filter.mask = CollisionLayer::World | CollisionLayer::Character;
    filter.ignoreOwner = id;
    filter.blockingOnly = true;
    vec3 start = physics_.body(o.physical).pose.position;
    auto move = physics_.moveAndSlide({start, a.radius, a.height}, delta, filter);
    vec3 accepted = start;
    int steps = std::max(1, int(std::ceil(glm::length(move.applied) / .08f)));
    for (int i = 1; i <= steps; ++i) {
        vec3 p = glm::mix(start, move.position, float(i) / steps);
        if (!Navigation::canStand(physics_, p - vec3(0, a.height * .5f, 0), a, navigation))
            break;
        accepted = p;
    }
    o.position = accepted;
    syncPose(o);
    return o.position;
}
vec3 World::rootMotion(uint32_t id, vec3 localDelta, float deltaYaw) {
    if (!std::isfinite(deltaYaw))
        throw std::invalid_argument("Invalid root rotation");
    auto rotation = entity(id).rotation;
    vec3 p = moveCharacter(id, rotation * localDelta);
    auto& o = mutableObject(id);
    o.rotation = glm::normalize(rotation * glm::angleAxis(deltaYaw, vec3(0, 1, 0)));
    syncPose(o);
    return p;
}
BodyMoveResult World::moveBody(uint32_t id, vec3 delta, quat targetRotation, uint32_t mask) {
    const auto& o = entity(id);
    const auto& body = physics_.body(o.physical);
    QueryFilter filter;
    filter.mask = mask;
    filter.ignoreOwner = id;
    filter.blockingOnly = true;
    auto result = physics_.moveAndSlide({body.shape, body.pose}, delta, targetRotation, filter);
    setTransform(id, result.pose);
    return result;
}
float World::setCharacterHeight(uint32_t id, float height) {
    auto& o = mutableObject(id);
    float before = o.position.y;
    physics_.resizeCharacter(o.physical, height);
    o.position = physics_.body(o.physical).pose.position;
    auto animation = animations_.find(id);
    if (animation != animations_.end())
        animation->second.rootOffset.y += before - o.position.y;
    syncPose(o);
    return physics_.body(o.physical).shape.height();
}
BodyHandle World::addAnimationCollider(uint32_t id, uint32_t joint, const ColliderShape& shape,
                                       PhysicsPose local, bool blocking) {
    auto& o = mutableObject(id);
    if (joint >= o.joints.size())
        throw std::out_of_range("Publish animation joints before adding collider");
    auto h = animationCollision_.bind(id, joint, shape, local, blocking);
    syncPose(o);
    animationCollision_.setEnabled(id, o.enabled);
    return h;
}
void World::setAnimationJoints(uint32_t id, std::vector<PhysicsPose> joints) {
    auto& o = mutableObject(id);
    animationCollision_.update(id, {o.position, o.rotation}, joints);
    o.joints = std::move(joints);
}
void World::attachAnimation(uint32_t id, std::shared_ptr<const animation::Skeleton> skeleton,
                            std::unique_ptr<animation::Solver> solver, bool applyRoot, vec3 offset) {
    attachAnimationInstance(id, std::make_unique<animation::Instance>(std::move(skeleton), std::move(solver)),
                            applyRoot, offset);
}
void World::attachAnimation(uint32_t id, const animation::Asset& asset, bool applyRoot, vec3 offset) {
    attachAnimationInstance(id, std::make_unique<animation::Instance>(asset), applyRoot, offset);
    if (!asset.header().id.empty())
        animations_.at(id).asset = asset.reference();
}
void World::attachAnimationInstance(uint32_t id, std::unique_ptr<animation::Instance> instance,
                                    bool applyRoot, vec3 offset) {
    const auto& o = entity(id);
    if (o.staticMesh)
        throw std::invalid_argument("Detach StaticMesh before attaching animation");
    if (applyRoot)
        (void)agent(id);
    if (!o.joints.empty() && o.joints.size() != instance->skeleton().size())
        throw std::invalid_argument("Detach animation before changing skeleton layout");
    auto previous = animations_.find(id);
    if (previous != animations_.end()) {
        const auto& before = previous->second.instance->skeleton().joints();
        const auto& after = instance->skeleton().joints();
        if (before.size() != after.size())
            throw std::invalid_argument("Detach animation before changing skeleton layout");
        for (size_t i = 0; i < before.size(); ++i)
            if (before[i].name != after[i].name || before[i].parent != after[i].parent)
                throw std::invalid_argument("Detach animation before changing skeleton layout");
    }
    animations_[id] = {std::move(instance), {}, {}, applyRoot, offset};
}
void World::setAnimationAttribute(uint32_t id, const std::string& key, const std::string& value) {
    animations_.at(id).instance->setAttribute(key, value);
}
AnimationInspection World::inspectAnimation(uint32_t id) const {
    auto found = animations_.find(id);
    if (found == animations_.end() || !entity(id).enabled)
        return {};
    const auto& instance = *found->second.instance;
    return {id, entity(id).name, instance.solverLabel(), instance.schema(), instance.attributes()};
}
void World::detachAnimation(uint32_t id) {
    auto& o = mutableObject(id);
    animations_.erase(id);
    animationCollision_.remove(id);
    o.joints.clear();
}
void World::setSkinnedMesh(uint32_t id, std::shared_ptr<const SkinnedMesh> mesh) {
    entity(id);
    auto& a = animations_.at(id);
    const auto& joints = a.instance->skeleton().joints();
    std::vector<unsigned> mapping;
    for (const auto& binding : mesh->bindings) {
        auto it = std::find_if(joints.begin(), joints.end(),
                               [&](const animation::Joint& j) { return j.name == binding.joint; });
        if (it == joints.end())
            throw std::invalid_argument("Mesh binding absent from animation skeleton: " + binding.joint);
        mapping.push_back(unsigned(it - joints.begin()));
    }
    a.mesh = std::move(mesh);
    a.meshJoints = std::move(mapping);
}
void World::setAnimationInput(uint32_t id, animation::Input input) {
    entity(id);
    animations_.at(id).input = std::move(input);
}
void World::resetAnimation(uint32_t id) {
    entity(id);
    animations_.at(id).instance->reset();
}
void World::setAnimationSolver(uint32_t id, std::unique_ptr<animation::Solver> solver) {
    entity(id);
    animations_.at(id).instance->setSolver(std::move(solver));
    animations_.at(id).asset.reset();
}
const animation::Output& World::animationOutput(uint32_t id) const {
    entity(id);
    return animations_.at(id).instance->output();
}
void World::updateAnimations(float dt) {
    CpuScope scope("Animation Evaluation / Root Motion / Joint Colliders");
    for (auto& entry : animations_) {
        auto& o = mutableObject(entry.first);
        auto& a = entry.second;
        if (!o.enabled)
            continue;
        animation::Transform objectRoot{o.position, o.rotation};
        auto root = animation::compose(objectRoot, {a.rootOffset, quat(1, 0, 0, 0)});
        const auto& output = a.instance->evaluate(dt, root, a.input);
        if (a.applyRootMotion) {
            auto forward = output.rootMotion.rotation * vec3(0, 0, 1);
            float yaw = std::atan2(forward.x, forward.z);
            // Offset origin must stay attached when root yaw changes.
            vec3 delta =
                output.rootMotion.position + a.rootOffset - output.rootMotion.rotation * a.rootOffset;
            rootMotion(o.id, delta, yaw);
        }
        auto model = a.instance->skeleton().toModel(output.localPose);
        std::vector<PhysicsPose> joints;
        joints.reserve(model.size());
        for (const auto& p : model)
            joints.push_back({p.position + a.rootOffset, p.rotation});
        setAnimationJoints(o.id, std::move(joints));
    }
}
std::vector<vec3> World::findPath(uint32_t id, vec3 target) const {
    return Navigation::findPath(physics_, feet(id), target, agent(id), navigation);
}
static vec3 rayDirection(const Camera& camera, float x, float y, const Input& input) {
    auto inv = glm::inverse(camera.projection(float(std::max(input.width, 1u)) / std::max(input.height, 1u)) *
                            camera.view());
    auto v = inv * vec4(2 * x / std::max(input.width, 1u) - 1, 2 * y / std::max(input.height, 1u) - 1, 1, 1);
    return glm::normalize(vec3(v) / v.w - camera.eye());
}
std::optional<vec3> World::groundAt(float x, float y, const Input& input) const {
    QueryFilter filter;
    filter.walkableOnly = true;
    filter.mask = CollisionLayer::World;
    auto hit = physics_.raycast(camera.eye(), rayDirection(camera, x, y, input), 160, filter);
    if (!hit)
        return {};
    return hit->position;
}
uint32_t World::pick(float x, float y, const Input& input) const {
    QueryFilter filter;
    filter.pickableOnly = true;
    auto hit = physics_.raycast(camera.eye(), rayDirection(camera, x, y, input), 160, filter);
    if (!hit || !hit->owner || hit->owner > objects_.size())
        return 0;
    const auto& o = entity(hit->owner);
    return o.interactable || o.id == playerId ? o.id : 0;
}
Frame World::snapshot(const Input& input, uint64_t tick, double time, int debug, bool physicsDebug) {
    CpuScope scope("World Snapshot");
    Frame f;
    for (const auto& object : objects_)
        if (object.alive && object.staticMesh)
            f.staticMeshes.push_back({object.proxy, object.staticMesh});
    f.textures = textures;
    f.animationInspection = inspectAnimation(selected);
    for (const auto& entry : animations_) {
        const auto& o = entity(entry.first);
        if (!o.enabled)
            continue;
        const auto& skeleton = entry.second.instance->skeleton();
        auto model = skeleton.toModel(entry.second.instance->output().localPose);
        animation::Transform root{o.position, o.rotation};
        root = animation::compose(root, {entry.second.rootOffset, quat(1, 0, 0, 0)});
        Frame::SkeletonPose pose;
        pose.owner = o.id;
        for (auto& p : model) {
            p = animation::compose(root, p);
            pose.jointWorld.push_back(glm::translate(mat4(1), p.position) * glm::mat4_cast(p.rotation));
        }
        const auto& a = entry.second;
        if (a.mesh) {
            Frame::Skin skin;
            skin.slot = o.proxy;
            skin.mesh = a.mesh;
            for (size_t j = 0; j < a.meshJoints.size(); ++j)
                skin.palette.push_back(pose.jointWorld[a.meshJoints[j]] * a.mesh->bindings[j].inverseBind);
            f.skins.push_back(std::move(skin));
        }
        if (physicsDebug)
            for (size_t j = 0; j < model.size(); ++j) {
                int parent = skeleton.joints()[j].parent;
                if (parent >= 0)
                    f.physicsLines.push_back({model[parent].position, model[j].position, {.3f, 1, .5f}});
            }
        f.skeletons.push_back(std::move(pose));
    }
    // The proxy array is already the authoritative render state; publishing it is a flat
    // copy of trivially copyable values, and the delta tells the renderer which of those
    // slots it actually has to touch.
    f.proxies = scene_.proxies();
    f.delta = scene_.publish();
    if (hovered && hovered <= objects_.size())
        f.hoveredName = objects_[hovered - 1].name;
    QueryFilter obstacles;
    obstacles.blockingOnly = true;
    for (auto handle : physics_.bodies(obstacles)) {
        const auto& b = physics_.body(handle);
        if (b.walkable || b.owner == playerId)
            continue;
        auto box = physics_.bounds(handle);
        f.mapObstacles.push_back({(box.min + box.max) * .5f, box.max - box.min});
    }
    f.materials = materials;
    f.lights = lights;
    f.camera = camera;
    f.input = input;
    if (playerId)
        f.player = entity(playerId).position;
    f.selected = selected;
    f.hovered = hovered;
    f.locomotion = state;
    f.message = message;
    f.destination = destination;
    f.hasDestination = hasDestination;
    f.path = path;
    f.tick = tick;
    f.time = time;
    f.resetHistory = resetHistory;
    f.debugView = debug;
    if (physicsDebug)
        appendPhysicsDebug(physics_, f);
    resetHistory = false;
    return f;
}
} // namespace afterlight
