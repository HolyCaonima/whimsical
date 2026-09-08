#include "TestGameplay.h"
#include "core/World.h"
#include "ecs/DataComponent.h"
#include "core/FrameMailbox.h"
#include "scene/ScenePersistence.h"
#include "scripting/RuntimeHost.h"
#include "render/RangeAllocator.h"
#include <iostream>
#include <limits>
using namespace afterlight;
static void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> static void rejects(F f, const char* message) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}
static bool near(vec3 a, vec3 b) {
    return glm::distance(a, b) < .0001f;
}
struct Counter {
    int value;
};
static void composition() {
    World w;
    w.resources.materials.emplace_back();
    auto data = w.create("Data");
    w.add<Counter>(data, Counter{42});
    check(!w.has<Transform>(data) && w.physics().size() == 0 && w.renderScene().capacity() == 0,
          "Data entity must allocate no spatial or render resources");
    check(w.get<Counter>(data).value == 42, "Independent component storage");
    auto visual = w.create("Visual");
    w.transforms.add(visual, {{2, 1, 0}});
    w.render.add(visual, {});
    auto physical = w.create("Collider");
    w.transforms.add(physical, {{4, 1, 0}});
    w.motion.add(physical, {});
    check(w.registry().view<Transform, Renderable>().size() == 1 && w.physics().size() == 1,
          "Queries must select independently composed capabilities");
    auto baseline = w.snapshot({}, 0, 0, 0);
    auto revision = w.physics().revision();
    w.render.setVisible(visual, false);
    w.render.setVisualPose(visual, {1, 2, 3}, {2, 3, 4});
    auto changed = w.snapshot({}, 1, 0, 0);
    check(w.physics().revision() == revision && changed.delta.structural.empty() &&
              changed.delta.topology == baseline.delta.topology,
          "Visual changes must not touch physics or geometry topology");
    rejects([&] { w.transforms.setParent(visual, data); }, "Parent requires Transform");
    rejects([&] { w.remove<Transform>(physical); }, "Removing a required transform must fail");
    auto handle = w.get<Collider>(physical).body;
    w.remove<Collider>(physical);
    check(!w.physics().contains(handle) && w.has<Transform>(physical),
          "Collider removal releases only its body");
    w.motion.add(physical, {});
    check(!w.physics().contains(handle), "Reattached collider cannot alias stale body handle");
    auto slot = w.get<Renderable>(visual).slot;
    w.remove<Renderable>(visual);
    check(!w.renderScene().proxy(slot).live && w.has<Transform>(visual), "Render removal leaves Transform");
    w.render.add(visual, {});
    check(w.get<Renderable>(visual).slot == slot, "Render slot reuse is bounded");
    auto old = visual;
    w.destroy(visual);
    for (int i = 0; i < 2000; ++i) {
        auto e = w.create();
        w.destroy(e);
    }
    check(!w.registry().contains(old) && w.registry().size() == 2,
          "Destroyed IDs never alias; no entity tombstones");
    rejects([&] { w.transforms.add(old); }, "Stale entity cannot gain a component");
    w.clearScene();
    check(w.registry().size() == 0 && w.physics().size() == 0, "Scene clear releases all capabilities");
    check(w.create() > old, "Scene reload preserves runtime identity sequence");
}
static void hierarchy() {
    World w;
    w.resources.materials.emplace_back();
    auto child = w.create("Child"), parent = w.create("Parent"), unrelated = w.create("Unrelated");
    w.transforms.add(child, {{1, 0, 0}});
    w.transforms.add(parent, {{3, 2, 0}, glm::angleAxis(Pi * .5f, vec3(0, 1, 0))});
    w.transforms.add(unrelated);
    w.render.add(child, {});
    w.render.add(parent, {});
    w.render.add(unrelated, {});
    w.motion.add(child, {});
    w.transforms.setParent(child, parent, false);
    auto expected = vec3(3, 2, -1);
    check(near(w.get<Transform>(child).world.position, expected),
          "Parent rotation composes local translation");
    check(near(w.physics().body(w.get<Collider>(child).body).pose.position, expected),
          "Hierarchy propagates to spatial index before queries");
    w.animation.setAnimationJoints(child, {{{0, 1, 0}}});
    w.remove<Animator>(child);
    check(w.has<JointPose>(child), "Removing an absent animator must preserve manual joints");
    auto joint = w.animation.addAnimationCollider(child, 0, ColliderShape::box(vec3(.1f)), {}, false);
    check(w.has<JointColliders>(child), "Joint collision is a queryable entity capability");
    auto before = w.snapshot({}, 0, 0, 0);
    auto pose = w.get<Transform>(parent).world;
    pose.position.x += 2;
    w.transforms.setTransform(parent, pose);
    auto after = w.snapshot({}, 1, 0, 0);
    check(after.delta.moved.size() == 2 && after.delta.structural.empty() && after.delta.attributes.empty(),
          "Parent move dirties only its rendered subtree");
    check(near(w.physics().body(joint).pose.position, expected + vec3(2, 1, 0)),
          "Attached joint collider follows hierarchy");
    check(near(before.proxies[w.get<Renderable>(child).slot].transform.position, expected),
          "Published snapshots remain immutable");
    rejects([&] { w.transforms.setParent(parent, child); }, "Hierarchy cycles must fail before mutation");
    auto local = w.get<Transform>(child).local;
    rejects([&] { w.transforms.setLocal(child, {{std::numeric_limits<float>::quiet_NaN(), 0, 0}}); },
            "Reject invalid local pose");
    check(w.get<Transform>(child).local.position == local.position, "Rejected write preserves authored pose");
    w.setEnabled(parent, false);
    check(!w.enabled(child) && !w.physics().body(joint).enabled,
          "Enabled state inherits without overwriting child's local flag");
    w.setEnabled(child, false);
    w.setEnabled(parent, true);
    check(!w.enabled(child), "Child's own disabled state survives parent enable");
    w.setEnabled(child, true);
    auto worldPose = w.get<Transform>(child).world;
    w.transforms.setParent(child, 0);
    check(near(w.get<Transform>(child).world.position, worldPose.position), "Unparent keeps world transform");
    w.transforms.setParent(child, parent);
    w.resources.references["child"] = w.get<Identity>(child).persistentId;
    w.destroy(parent);
    check(!w.registry().contains(child) && !w.physics().contains(joint) && w.registry().contains(unrelated),
          "Subtree destruction releases children and attachments only");
    check(w.resources.references.empty(),
          "Destruction clears persistent references and selection");
}
class RestSolver : public animation::Solver {
  public:
    void reset(const animation::Context&) override {}
    void evaluate(const animation::Context& c, animation::Output& o) override {
        o.localPose = c.skeleton.restPose();
    }
};
static void animationLifetime() {
    World w;
    auto e = w.create();
    w.transforms.add(e);
    auto skeleton = std::make_shared<animation::Skeleton>(std::vector<animation::Joint>{{"root", -1, {}}});
    w.animation.attachAnimation(e, skeleton, std::make_unique<RestSolver>());
    check(!w.has<Collider>(e) && !w.has<Renderable>(e),
          "Headless animation needs neither rendering nor physics");
    w.update(.1f);
    rejects([&] { w.animation.setAnimationJoints(e, {}); },
            "Manual poses cannot overwrite solver-owned state");
    rejects([&] { w.remove<JointPose>(e); }, "Solved joint removal respects dependency");
    w.remove<Animator>(e);
    check(!w.has<JointPose>(e), "Animator removal releases derived pose");
    RuntimeHost scripts(w, testAssets());
    scripts.initialize(testProject());
    auto player = testPlayer(w);
    auto before = w.snapshot({}, 1, 0, 0);
    w.motion.setCharacterHeight(player, 1.2f);
    auto crouched = w.snapshot({}, 1, 0, 0);
    check(near(vec3(before.skeletons[0].jointWorld[0][3]), vec3(crouched.skeletons[0].jointWorld[0][3])),
          "Stance changes preserve skeleton anchor before the next animation tick");
    w.motion.setCharacterHeight(player, 2);
    before = w.snapshot({}, 1, 0, 0);
    auto mesh = w.get<Skin>(player).mesh;
    w.setEnabled(player, false);
    w.update(.1f);
    auto disabled = w.snapshot({}, 2, 0, 0);
    check(disabled.skins.size() == before.skins.size() && disabled.delta.topology == before.delta.topology &&
              disabled.skins[0].mesh == before.skins[0].mesh &&
              disabled.skins[0].palette == before.skins[0].palette,
          "Disabled skin retains geometry and final pose");
    w.setEnabled(player, true);
    w.remove<Renderable>(player);
    check(w.has<Animator>(player) && !w.has<Skin>(player), "Removing rendering retains headless animation");
    w.update(.1f);
    w.render.add(player, {});
    w.animation.setSkinnedMesh(player, mesh);
    auto bound = w.snapshot({}, 3, 0, 0);
    check(bound.skins.size() == before.skins.size() && bound.delta.topology > disabled.delta.topology,
          "Reattached skin publishes geometry topology");
    auto body = w.get<Collider>(player).body;
    rejects([&] { w.remove<Collider>(player); }, "Root motion retains required collider");
    w.remove<Animator>(player);
    w.remove<Collider>(player);
    check(!w.physics().contains(body) && w.has<Renderable>(player),
          "Component dependency removal leaves independent render capability");
}
class ObservedSolver : public animation::Solver {
    float& observed;

  public:
    explicit ObservedSolver(float& value) : observed(value) {}
    void reset(const animation::Context&) override {}
    void evaluate(const animation::Context& c, animation::Output& o) override {
        observed = c.root.position.x;
        o.localPose = c.skeleton.restPose();
        o.rootMotion.position.x = c.dt;
    }
};
static void updateDependencies() {
    World w;
    auto child = w.create(), parent = w.create(), ground = w.create();
    w.transforms.add(child, {{2, 0, 0}});
    w.transforms.add(parent, {{0, 1, 0}});
    w.transforms.add(ground, {{0, -.25f, 0}});
    w.transforms.setParent(child, parent, false);
    PhysicsBody floor;
    floor.shape = ColliderShape::box({10, .25f, 10});
    floor.walkable = true;
    w.motion.add(ground, floor);
    PhysicsBody body;
    body.shape = ColliderShape::capsule(.4f, 2);
    body.layer = CollisionLayer::Character;
    w.motion.add(parent, body);
    auto skeleton = std::make_shared<animation::Skeleton>(std::vector<animation::Joint>{{"root", -1, {}}});
    float seenChild = -1, seenParent = -1;
    w.animation.attachAnimation(child, skeleton, std::make_unique<ObservedSolver>(seenChild));
    w.animation.attachAnimation(parent, skeleton, std::make_unique<ObservedSolver>(seenParent));
    w.motion.bindRootMotion(parent, {RootMotionBinding::Mode::Grounded});
    w.update(.1f);
    check(std::abs(seenChild - 2.1f) < .0001f && seenParent == 0,
          "Animation evaluates hierarchy ancestors before children, independent of creation order");
    w.setEnabled(parent, false);
    w.update(.1f);
    check(std::abs(seenChild - 2.1f) < .0001f, "Inherited disable skips descendant animation evaluation");
}
static void persistenceAndScripts() {
    World w;
    auto shader = testAssets().load<ShaderAsset>(AssetPath("/Game/shaders/Standard"));
    auto directory = std::filesystem::path(AFTERLIGHT_ROOT) / "build" / ("ecs-" + newPersistentId());
    AssetManager local{Project::create(directory, "ECS lifecycle").content()};
    registerEngineAssets(local);
    auto header = shader->header();
    header.storage = PayloadStorage::Inline;
    header.source.clear();
    local.save(shader->reference().path, header, shader->source);
    MaterialDefinition material;
    material.shader = local.reference(shader->reference().path);
    w.resources.materials.push_back(material.resolve(local));
    RuntimeHost scripts(w, local);
    scripts.execute(R"JS(
var data=Engine.create({name:'Data',components:{data:{health:10}}});
var value=Engine.data(data);value.health=20;Engine.setData(data,value);
if(Engine.data(data).health!==20)throw Error('entity data');
var parent=Engine.create({name:'Parent',components:{transform:{position:[3,0,0]}}});
var child=Engine.create({name:'Child',components:{transform:{position:[1,0,0]},render:{material:0}}});
if(!Engine.component(child,'render').castShadow)throw Error('default shadow visibility');
var appearance=Engine.component(child,'render');appearance.castShadow=false;
Engine.setComponent(child,'render',appearance);
Engine.parent(child,parent,false);
if(Engine.position(child).x!==4)throw Error('hierarchy binding');
if(Engine.entities(['render']).length!==1||Engine.hasComponent(data,'transform'))throw Error('query');
Engine.addComponent(data,'transform',{position:[10,1,0]});
Engine.addComponent(data,'collider',{shape:{type:'box',halfExtents:[1,1,1]},blocking:true,pickable:true});
if(Engine.hasComponent(data,'render'))throw Error('collider recipe');
Engine.removeComponent(data,'collider');
Engine.removeComponent(data,'transform');
try{Engine.addComponent(data,'transform',{parent:'ffffffffffffffffffffffffffffffff'});throw Error('expected missing parent');}
catch(e){if(Engine.hasComponent(data,'transform'))throw Error('failed attach left a component');}
Engine.addComponent(child,'joints',[]);
var stale=Engine.create({components:{}});Engine.destroy(stale);
if(Engine.alive(stale))throw Error('stale');
Engine.enabled(parent,false);
)JS");
    auto document = ScenePersistence::capture(w, local);
    check(document.entities.size() == 3 && document.json().at("version").uint() == 7,
          "Persist live entities in component schema");
    auto decoded = SceneDocument::fromJson(Json::parse(document.json().dump()));
    check(decoded.json() == document.json(), "Optional component roundtrip is lossless");
    check(decoded.entities.back().components.find<SceneJoints>() &&
              decoded.entities.back().components.find<SceneJoints>()->poses.empty(),
          "Empty authored joint component retains presence");
    auto saved = ScenePersistence::save(w, local, AssetPath("/Game/Maps/Ecs"), "ECS");
    auto stale = w.registry().entities().back();
    ScenePersistence::load(w, local, saved.path);
    check(!w.registry().contains(stale) && w.physics().size() == 0,
          "Reload invalidates handles without inventing colliders");
    auto restored = ScenePersistence::capture(w, local);
    check(restored.json() == document.json(),
          "Save/load retains local transforms and inherited enabled flags");
    auto child = w.findObject(document.entities.back().id);
    check(near(w.get<Transform>(child).world.position, {4, 0, 0}) && !w.enabled(child),
          "Loaded hierarchy derives world transform and activation");
    check(!w.get<Renderable>(child).appearance.castShadow &&
              !w.renderScene().proxy(w.get<Renderable>(child).slot).attributes.castShadow,
          "Shadow visibility survives scripting, persistence and render extraction");
    auto invalid = restored;
    invalid.entities.back().components.find<SceneTransform>()->parent = newPersistentId();
    rejects([&] { invalid.validate(); }, "Missing parent must fail validation");
    invalid = restored;
    invalid.entities[1].components.find<SceneTransform>()->parent = invalid.entities.back().id;
    rejects([&] { invalid.validate(); }, "Serialized hierarchy cycle must fail validation");
}
struct Energy {
    int value;
};
static void componentContracts() {
    static int preparations = 0;
    auto contract = dataComponent<Energy>(
        "energy", [](const Json& j) { return Energy{int(j.at("value").number())}; },
        [](const Energy& v) { return Json{{"value", v.value}}; },
        [](const Energy& v) {
            if (v.value < 0)
                throw std::invalid_argument("Negative energy");
        });
    contract.dependencies = {{"transform"}};
    contract.validate = [](const ComponentSet& set, const std::any& value) {
        if (std::any_cast<const Energy&>(value).value > 50 && !set.contains("interactable"))
            throw std::invalid_argument("High energy requires interactable");
    };
    auto prepare = contract.prepare;
    contract.prepare = [prepare](const World& world, Entity entity, const std::any& value,
                                 AssetManager& assets) {
        ++preparations;
        auto install = prepare(world, entity, value, assets);
        return [install, value](ComponentAccess& access) {
            install(access);
            if (std::any_cast<const Energy&>(value).value == 42)
                throw std::runtime_error("Injected failure after component installation");
        };
    };
    componentCatalog().add(std::move(contract));
    int notifications = 0;
    World w;
    w.onChange<Energy>([&](Entity) { ++notifications; });
    RuntimeHost scripts(w, testAssets());
    scripts.execute(R"JS(
var actor=Engine.create({name:'Contract',components:{energy:{value:3},transform:{}}});
if(Engine.component(actor,'energy').value!==3||Engine.entities(['energy']).length!==1)throw Error('registered codec/query');
var value=Engine.component(actor,'energy');value.value=20;
if(Engine.component(actor,'energy').value!==3)throw Error('authored copy');
)JS");
    auto e = w.registry().entities().front();
    check(notifications == 1, "One notification after component group commits");
    rejects([&] { w.edit<Energy>(e, [](Energy& v) { v.value = -1; }); },
            "Native edit shares component validation");
    rejects([&] { w.edit<Energy>(e, [](Energy& v) { v.value = 100; }); },
            "Native drafts share complete composition validation");
    check(w.get<Energy>(e).value == 3 && notifications == 1,
          "Rejected draft never changes state or publishes");
    w.edit<Energy>(e, [](Energy& v) { v.value = 7; });
    check(notifications == 2, "Native edit publishes through the same contract");
    rejects([&] { w.remove<Transform>(e); }, "Registered dependency participates in native removal");
    auto data = w.create("Unchanged");
    rejects([&] { w.add<Energy>(data, Energy{1}); }, "Native attach shares dependencies");
    scripts.execute(R"JS(
var empty=Engine.create({components:{data:{keep:true}}});
try { Engine.addComponents(empty,{transform:{},energy:{value:9},render:{material:999}}); throw Error('expected reject'); }
catch(e) { if(Engine.hasComponent(empty,'transform')||Engine.hasComponent(empty,'energy'))throw Error('partial preparation'); }
try { Engine.addComponents(empty,{transform:{},joints:[],jointColliders:[{joint:0,shape:{type:'box',halfExtents:[1,1,1]},local:{},blocking:false}]}); throw Error('expected binding reject'); }
catch(e) { if(Engine.hasComponent(empty,'transform')||Engine.hasComponent(empty,'joints')||Engine.hasComponent(empty,'jointColliders'))throw Error('partial installation'); }
try { Engine.addComponents(empty,{transform:{},energy:{value:42}}); throw Error('expected late failure'); }
catch(e) { if(Engine.hasComponent(empty,'transform')||Engine.hasComponent(empty,'energy'))throw Error('failed installer survived rollback'); }
if(!Engine.data(empty).keep)throw Error('rollback changed preexisting data');
)JS");
    check(w.physics().size() == 0, "Failed component group releases its derived bindings");
    auto document = ScenePersistence::capture(w, testAssets());
    auto decoded = SceneDocument::fromJson(Json::parse(document.json().dump()));
    check(decoded.entities.front().components.find<Energy>()->value == 7,
          "Registered component persists without SceneEntity edits");
    auto revision = w.physics().revision();
    w.motion.add(e, {});
    {
        auto batch = w.changes();
        w.transforms.setLocal(e, {{1, 0, 0}});
        w.transforms.setLocal(e, {{2, 0, 0}});
        rejects([&] { (void)w.snapshot({}, 0, 0, 0); },
                "Extraction cannot observe uncommitted backend state");
        batch.commit();
    }
    check(w.physics().revision() == revision + 2 &&
              near(w.physics().body(w.get<Collider>(e).body).pose.position, {2, 0, 0}),
          "Batch commits only the final derived pose");
    auto oldBody = w.get<Collider>(e).body;
    auto id = w.get<Identity>(e).persistentId;
    auto directory =
        std::filesystem::path(AFTERLIGHT_ROOT) / "build" / ("component-contract-" + newPersistentId());
    AssetManager local{Project::create(directory, "Component contracts").content()};
    registerEngineAssets(local);
    auto saved = ScenePersistence::save(w, local, AssetPath("/Game/Maps/Contracts"), "Contracts");
    preparations = 0;
    ScenePersistence::load(w, local, saved.path);
    check(preparations == 1 && !w.registry().contains(e) && !w.physics().contains(oldBody),
          "Scene commits prepared components once and cannot alias prior identities or body handles");
    e = w.findObject(id);
    check(w.get<Energy>(e).value == 7 && notifications == 5,
          "Scene commit publishes new components through World subscriptions");
    w.destroy(e);
    check(notifications == 6, "Entity destruction uses registered component lifecycle");
}
static void independentPoseAndMotion() {
    World w;
    w.resources.materials.emplace_back();
    auto layout = std::make_shared<animation::Skeleton>(std::vector<animation::Joint>{{"root", -1, {}}});
    auto e = w.create();
    w.transforms.add(e);
    w.render.add(e, {});
    w.animation.setAnimationJoints(e, {{{0, 1, 0}}}, layout);
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->bindings.push_back({"root", mat4(1)});
    w.animation.setSkinnedMesh(e, mesh);
    check(!w.has<Animator>(e) && w.snapshot({}, 0, 0, 0).skins.size() == 1,
          "Named manual pose drives skin without Animator");
    auto before = w.get<JointPose>(e).model;
    rejects([&] { w.animation.setAnimationJoints(e, {}); }, "Bound joint layout cannot shrink");
    check(w.get<JointPose>(e).model.size() == before.size(), "Rejected pose keeps its derived state");
    w.remove<JointPose>(e);
    check(!w.has<Skin>(e) && w.has<Renderable>(e),
          "Removing pose cascades its skin while preserving render capability");
    float observed = 0;
    w.animation.attachAnimation(e, layout, std::make_unique<ObservedSolver>(observed));
    w.motion.bindRootMotion(e, {});
    w.update(.25f);
    check(!w.has<Collider>(e) && near(w.get<Transform>(e).world.position, {.25f, 0, 0}),
          "Transform root motion needs no collider or ground");
    w.remove<RootMotionBinding>(e);
    PhysicsBody body;
    body.shape = ColliderShape::box(vec3(.2f));
    body.motion = BodyMotion::Kinematic;
    w.motion.add(e, body);
    w.motion.bindRootMotion(e, {RootMotionBinding::Mode::Kinematic});
    w.update(.25f);
    check(near(w.get<Transform>(e).world.position, {.5f, 0, 0}),
          "Box collider can consume kinematic root motion");
    w.remove<Animator>(e);
    check(!w.has<RootMotionBinding>(e) && !w.has<JointPose>(e) && w.has<Collider>(e),
          "Producer removal releases owned pose and motion consumer");
}
static void commitAndRecovery() {
    Changes changes;
    bool fail = true;
    int attempts = 0, notices = 0;
    changes.subscribe<Counter>([&](Entity) {
        ++attempts;
        rejects([&] { changes.requireCommitted(); }, "Queries cannot run during derived synchronization");
        if (fail)
            throw std::runtime_error("Backend unavailable");
    });
    changes.observe<Counter>([&](Entity) {
        changes.requireCommitted();
        ++notices;
    });
    {
        Changes::Batch batch(changes);
        changes.mark<Counter>(1);
        rejects([&] { changes.flush(); }, "Cannot flush through an enclosing batch");
        rejects([&] { batch.commit(); }, "Synchronization failure reaches caller");
        rejects([&] { batch.commit(); }, "A failed commit still closes its batch exactly once");
    }
    rejects([&] { changes.requireCommitted(); }, "Failed synchronization keeps queries closed");
    fail = false;
    changes.flush();
    check(attempts == 2 && notices == 1, "Retry completes lost work before notifying observers");
    {
        Changes::Batch abandoned(changes);
        changes.mark<Counter>(1);
    }
    rejects([&] { changes.requireCommitted(); }, "Unfinished batch requires explicit recovery");
    changes.flush();

    World w;
    w.resources.materials.emplace_back();
    int identities = 0;
    w.onChange<Identity>([&](Entity) { ++identities; });
    SceneEntity invalid;
    invalid.components.set(SceneTransform{});
    SceneRender render;
    render.appearance.material = 999;
    invalid.components.set(render);
    rejects([&] { ScenePersistence::createEntity(w, invalid, testAssets()); }, "Invalid creation rejects");
    check(w.registry().size() == 0 && identities == 0, "Failed creation has no published lifetime");
    auto e = w.create();
    w.transforms.add(e);
    w.render.add(e, {});
    PhysicsBody body;
    body.shape = ColliderShape::capsule(.3f, 2);
    w.motion.add(e, body);
    bool throwObserver = true;
    int completed = 0;
    w.onChange<Transform>([&](Entity changed) {
        if (!w.has<Transform>(changed))
            return;
        check(near(w.physics().body(w.get<Collider>(changed).body).pose.position,
                   w.get<Transform>(changed).world.position),
              "Observers see synchronized physics");
        rejects([&] { w.transforms.setLocal(changed, {{99, 0, 0}}); }, "Observers cannot mutate components");
        if (throwObserver)
            throw std::runtime_error("Observer failed");
    });
    w.onChange<Transform>([&](Entity) { ++completed; });
    rejects([&] { w.transforms.setLocal(e, {{3, 1, 0}}); }, "Observer failure is reported after commit");
    check(near(w.get<Transform>(e).world.position, {3, 1, 0}) && completed == 1,
          "Observer failure neither rolls back state nor skips peers");
    w.commitChanges();
    check(completed == 1, "Committed observers are never replayed");
    throwObserver = false;
    {
        auto batch = w.changes();
        w.transforms.setLocal(e, {{4, 1, 0}});
        rejects([&] { w.motion.moveBody(e, {}, quat(1, 0, 0, 0)); }, "Motion cannot query stale backends");
        batch.commit();
    }
}
static void editsAndLights() {
    World w;
    auto parent = w.create(), e = w.create();
    w.transforms.add(parent, {{3, 2, 1}, glm::angleAxis(1.f, vec3(0, 1, 0))});
    w.transforms.add(e, {{1, 0, 0}});
    w.transforms.setParent(e, parent, false);
    w.add<LightComponent>(e, LightComponent{{1, .5f, .2f}, 10, .2f});
    auto frame = w.snapshot({}, 0, 0, 0);
    check(frame.lightEntities == std::vector<Entity>{e} &&
              near(vec3(frame.lights[0].positionRadius), w.get<Transform>(e).world.position) &&
              w.physics().size() == 0 && w.renderScene().capacity() == 0,
          "Light follows hierarchy without allocating collider or render geometry");
    RuntimeHost script(w, testAssets());
    script.execute(R"JS(
var light = Engine.entities(['light'])[0];
Engine.setComponent(light, 'light', {color:[.2,.4,1],intensity:22,radius:.3});
if(Engine.component(light,'light').intensity!==22)throw Error('light edit');
try { Engine.setComponent(light,'light',{intensity:-1}); throw Error('expected invalid light'); }
catch(e) { if(Engine.component(light,'light').intensity!==22)throw Error('invalid draft escaped'); }
)JS");
    auto saved = ScenePersistence::capture(w, testAssets());
    check(saved.entities[1].components.find<LightComponent>()->intensity == 22 &&
              !saved.json().contains("lights"),
          "Light uses the same authored component in script and persistence");
    w.setEnabled(parent, false);
    check(w.snapshot({}, 1, 0, 0).lights.empty(), "Parent disable removes emitted light");
    w.setEnabled(parent, true);
    w.resources.materials.emplace_back();
    w.render.add(e, {});
    w.motion.add(e, {});
    auto slot = w.get<Renderable>(e).slot;
    auto handle = w.get<Collider>(e).body;
    SceneRender appearance;
    appearance.appearance.scale = {2, 3, 4};
    w.set(e, appearance, testAssets());
    SceneCollider collider;
    collider.shape = ColliderShape::box({2, 1, 3});
    w.set(e, collider, testAssets());
    check(w.get<Renderable>(e).slot == slot && w.get<Collider>(e).body == handle &&
              near(w.physics().body(handle).shape.halfExtents, {2, 1, 3}),
          "Descriptor edits retain independent backend bindings");
    auto replacement = w.create();
    w.transforms.add(replacement);
    w.add<LightComponent>(replacement, LightComponent{});
    w.destroy(parent);
    auto next = w.snapshot({}, 2, 0, 0);
    check(next.lights.size() == frame.lights.size() && next.lightEntities != frame.lightEntities &&
              !w.physics().contains(handle),
          "Same-count light replacement changes history identity and releases subtree");
}
class RecoveringSolver : public animation::Solver {
  public:
    bool invalid = true;
    int resets = 0;
    void reset(const animation::Context&) override {
        ++resets;
    }
    void evaluate(const animation::Context& c, animation::Output& output) override {
        output.localPose = c.skeleton.restPose();
        output.rootMotion.position = {1, 0, 0};
        if (invalid)
            output.localPose[0].position.x = std::numeric_limits<float>::quiet_NaN();
    }
};
static void animationCommitAndVisualSpace() {
    World w;
    w.resources.materials.emplace_back();
    auto e = w.create();
    w.transforms.add(e, {{2, 0, 0}, glm::angleAxis(.6f, vec3(0, 1, 0))});
    auto skeleton = std::make_shared<animation::Skeleton>(std::vector<animation::Joint>{{"root", -1, {}}});
    auto solver = std::make_unique<RecoveringSolver>();
    auto control = solver.get();
    w.animation.attachAnimation(e, skeleton, std::move(solver));
    w.motion.bindRootMotion(e, {});
    auto before = w.get<Transform>(e).world;
    rejects([&] { w.update(.1f); }, "Invalid solver output fails before component commit");
    check(near(w.get<Transform>(e).world.position, before.position) &&
              near(w.get<JointPose>(e).model[0].position, {}) &&
              near(w.animation.animationOutput(e).rootMotion.position, {}),
          "Failed animation preserves root, joints and accepted output");
    control->invalid = false;
    w.update(.1f);
    check(control->resets == 2 &&
              near(w.get<Transform>(e).world.position, before.position + before.rotation * vec3(1, 0, 0)),
          "Solver retry resets history from accepted state");
    w.render.add(e, {});
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->bindings.push_back({"root", mat4(1)});
    w.animation.setSkinnedMesh(e, mesh);
    w.render.setVisualPose(e, {0, 2, 0}, {2, 3, 4});
    auto frame = w.snapshot({}, 0, 0, 0);
    const auto& root = w.get<Transform>(e).world;
    auto expected = root.position + root.rotation * (vec3(0, 2, 0) + vec3(2, 3, 4));
    check(near(vec3(frame.skins[0].palette[0] * vec4(1, 1, 1, 1)), expected) &&
              near(vec3(frame.skeletons[0].jointWorld[0][3]), root.position),
          "Skin applies visual space while skeletal and collision space stay rigid");
    SceneRender incompatible;
    incompatible.mesh = AssetRef{};
    rejects([&] { w.set(e, incompatible, testAssets()); }, "Existing skin participates in update validation");
}
static void ranges() {
    RangeAllocator ranges;
    auto a = ranges.allocate(20), b = ranges.allocate(30), c = ranges.allocate(40);
    ranges.release(b, 30);
    check(ranges.allocate(10) == b && ranges.size() == 90,
          "Geometry holes reused without moving live ranges");
    ranges.release(b, 10);
    ranges.release(c, 40);
    check(ranges.size() == 20, "Adjacent freed geometry ranges coalesce");
    ranges.release(a, 20);
    check(ranges.size() == 0, "Geometry high-water tail is reclaimed");
}
static void legacyImport() {
    SceneDocument seed;
    MaterialDefinition material;
    material.shader = testAssets().reference(AssetPath("/Game/shaders/Standard"));
    seed.materials.push_back(material);
    auto legacy = seed.json();
    legacy["version"] = 3;
    legacy["lights"] = Json::array();
    Json object{{"id", newPersistentId()},
                {"name", "Legacy"},
                {"position", Json::array({2, 1, 0})},
                {"rotation", Json::array({0, 0, 0, 1})},
                {"enabled", true},
                {"interactable", false},
                {"render",
                 {{"shape", "box"},
                  {"scale", Json::array({1, 1, 1})},
                  {"offset", Json::array({0, 0, 0})},
                  {"animationScale", Json::array({1, 1, 1})},
                  {"material", 0},
                  {"visible", true}}},
                {"physics",
                 {{"shape", {{"type", "box"}, {"halfExtents", Json::array({.5, .5, .5})}}},
                  {"motion", "static"},
                  {"layer", 1},
                  {"blocking", false},
                  {"walkable", false},
                  {"pickable", false}}},
                {"animation", Json()},
                {"joints", Json::array()},
                {"jointColliders", Json::array()}};
    legacy["objects"] = Json::array({object});
    legacy["player"] = object.at("id");
    auto migrated = SceneDocument::fromJson(legacy);
    check(migrated.references.at("player") == object.at("id").string() &&
              !migrated.json().contains("player"),
          "Legacy player imports as an ordinary named reference only");
    check(migrated.entities.size() == 1 && migrated.entities[0].components.find<SceneTransform>() &&
              migrated.entities[0].components.find<SceneRender>() &&
              migrated.entities[0].components.find<SceneCollider>() &&
              !migrated.entities[0].components.find<SceneJoints>() &&
              migrated.json().at("version").uint() == 8,
          "Legacy import preserves authored collider semantics while saving optional v6 components");
}
int main() {
    try {
        composition();
        hierarchy();
        animationLifetime();
        updateDependencies();
        persistenceAndScripts();
        componentContracts();
        independentPoseAndMotion();
        commitAndRecovery();
        editsAndLights();
        animationCommitAndVisualSpace();
        ranges();
        legacyImport();
        std::cout
            << "ECS composition, hierarchy, lifecycle, scripts, persistence and resource ranges passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
