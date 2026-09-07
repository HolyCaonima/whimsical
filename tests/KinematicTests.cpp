#include "TestEntities.h"
#include "TestProject.h"
#include "scene/ScenePersistence.h"
#include "scripting/ScriptRuntime.h"
#include <iostream>
#include <stdexcept>

using namespace afterlight;
static void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
static bool near(vec3 a, vec3 b, float tolerance = 1e-4f) {
    return glm::distance(a, b) < tolerance;
}
static bool sameRotation(quat a, quat b) {
    return std::abs(glm::dot(a, b)) > 1 - 1e-5f;
}
static BodyHandle box(PhysicsScene& scene, uint32_t owner, vec3 position, vec3 half) {
    PhysicsBody b;
    b.owner = owner;
    b.shape = ColliderShape::box(half);
    b.pose.position = position;
    return scene.create(b);
}
static QueryFilter blocking() {
    QueryFilter f;
    f.blockingOnly = true;
    return f;
}
static void continuousMotion() {
    PhysicsScene scene;
    auto floor = box(scene, 1, {0, -.2f, 0}, {50, .2f, 50});
    scene.setProperties(floor, CollisionLayer::World, true, true, false);
    auto wall = box(scene, 2, {0, 2, 0}, {.005f, 2, 30});
    ShapeQuery car{ColliderShape::box({.9f, .5f, 2}),
                    {{-8, .5f, 0}, glm::angleAxis(.4f, vec3(0, 1, 0))}};
    auto hit = scene.sweepShape(car, {30, 0, 0}, car.pose.rotation, blocking());
    float extent = .9f * std::cos(.4f) + 2 * std::sin(.4f);
    check(hit && hit->owner == 2 && std::abs(hit->distance - (8 - extent - .005f)) < .001f,
          "Rotated box must stop at its actual footprint against a 1 cm wall at high speed");
    check(near(hit->normal, {-1, 0, 0}), "Sweep contact normal must be in world space");
    auto slide = scene.moveAndSlide(car, {30, 0, 3}, car.pose.rotation, blocking());
    check(slide.blocked && slide.pose.position.x < -extent && slide.pose.position.z > 2.99f,
          "Box collision must retain tangential movement along the wall");
    check(scene.overlapShape({car.shape, slide.pose}, blocking()).empty(), "Box slide penetrated geometry");
    auto filter = blocking();
    filter.ignoreOwner = 2;
    check(!scene.sweepShape(car, {30, 0, 0}, car.pose.rotation, filter), "Shape queries must honor ignoreOwner");
    scene.setEnabled(wall, false);
    check(!scene.sweepShape(car, {30, 0, 0}, car.pose.rotation, blocking()),
          "Disabled obstacles must immediately leave generic shape queries");
    auto yaw = glm::angleAxis(1.7f, vec3(0, 1, 0));
    auto turn = scene.moveAndSlide(car, {0, 0, 8}, yaw, blocking());
    check(!turn.blocked && near(turn.pose.position, car.pose.position + vec3(0, 0, 8)) &&
              sameRotation(turn.pose.rotation, yaw),
          "Yaw rotation on a tangent floor must preserve full movement and rotation");
    scene.setEnabled(wall, true);
    ShapeQuery sideways{ColliderShape::capsule(.3f, 3),
                         {{-8, 3, 0}, glm::angleAxis(Pi * .5f, vec3(0, 0, 1))}};
    auto capsuleHit = scene.sweepShape(sideways, {30, 0, 0}, sideways.pose.rotation, blocking());
    check(capsuleHit && capsuleHit->owner == 2 && std::abs(capsuleHit->distance - 6.495f) < .001f,
          "Generic capsule sweep must use the capsule's actual orientation");
    scene.destroy(wall);
    PhysicsBody capsule;
    capsule.owner = 3;
    capsule.shape = ColliderShape::capsule(.5f, 3);
    capsule.pose.position = {0, 2, 0};
    scene.create(capsule);
    ShapeQuery cube{ColliderShape::box(vec3(.5f)), {{-8, 2, 0}, quat(1, 0, 0, 0)}};
    auto mixed = scene.sweepShape(cube, {30, 0, 0}, cube.pose.rotation, blocking());
    check(mixed && mixed->owner == 3 && std::abs(mixed->distance - 7) < .001f && mixed->normal.x < -.99f,
          "Box against capsule CCD must return the correct distance and normal");
    ShapeQuery capQuery{ColliderShape::capsule(.4f, 2), {{-8, 2, 0}, quat(1, 0, 0, 0)}};
    auto caps = scene.sweepShape(capQuery, {30, 0, 0}, capQuery.pose.rotation, blocking());
    check(caps && caps->owner == 3 && std::abs(caps->distance - 7.1f) < .001f,
          "Generic capsule against capsule CCD must preserve radii");
    cube.pose.position = {0, 2, 0};
    check(!scene.overlapShape(cube, blocking()).empty(), "Box/capsule penetration must be queryable");
}
static void rotationalMotion() {
    PhysicsScene scene;
    box(scene, 1, {1.5f, 1, 0}, {.05f, .5f, .05f});
    ShapeQuery bar{ColliderShape::box({.4f, .4f, 2}), {{0, 1, 0}, quat(1, 0, 0, 0)}};
    quat halfTurn(0, 0, 1, 0);
    check(scene.overlapShape(bar).empty() &&
              scene.overlapShape({bar.shape, {bar.pose.position, halfTurn}}).empty(),
          "Rotation test requires collision-free endpoints");
    auto hit = scene.sweepShape(bar, vec3(0), halfTurn, blocking());
    check(hit && hit->fraction > .1f && hit->fraction < .5f,
          "Rotational CCD must detect intermediate collision even with clear endpoints");
    auto stopped = scene.moveAndSlide(bar, vec3(0), halfTurn, blocking());
    check(stopped.blocked && near(stopped.pose.position, bar.pose.position) &&
              !sameRotation(stopped.pose.rotation, bar.pose.rotation) &&
              !sameRotation(stopped.pose.rotation, halfTurn),
          "Rotation-only movement must preserve the accepted partial rotation");
    check(scene.overlapShape({bar.shape, stopped.pose}, blocking()).empty(),
          "Rotation stop must not penetrate the obstacle");
    // Test arbitrary pitch/roll, not just a top-down yaw footprint.
    PhysicsScene ceiling;
    box(ceiling, 1, {0, 2.5f, 0}, {10, .05f, 10});
    ShapeQuery upright{ColliderShape::box({.3f, .3f, 2}), {{0, 1, 0}, quat(1, 0, 0, 0)}};
    auto pitch = glm::angleAxis(Pi * .5f, vec3(1, 0, 0));
    auto pitched = ceiling.moveAndSlide(upright, vec3(0), pitch, blocking());
    check(pitched.blocked && ceiling.overlapShape({upright.shape, pitched.pose}, blocking()).empty(),
          "Pitch rotation must sweep into ceilings without penetration");
    auto noChange = ceiling.moveAndSlide(upright, vec3(0), -upright.pose.rotation, blocking());
    check(!noChange.blocked && sameRotation(noChange.pose.rotation, upright.pose.rotation),
          "Antipodal quaternions must represent no physical rotation");
}
static void transformsAndScript() {
    World world;
    MaterialDefinition material;
    material.shader = testAssets().reference(AssetPath("/Game/shaders/Standard"));
    world.resources.materials.push_back(material.resolve(testAssets()));
    ScriptRuntime js(world, testAssets());
    js.execute(R"JS(
        function assert(value, message) { if (!value) throw new Error(message); }
        var wall = Engine.create({name:'wall',components:{transform:{position:[0,3,0]},render:{scale:[.01,6,20],material:0},collider:{shape:{type:'box',halfExtents:[0.005,3.0,10.0]},blocking:true,pickable:true}}});
        var body = Engine.create({name:'body',components:{transform:{position:[-5,3,0]},render:{scale:[2,1,4],material:0},collider:{shape:{type:'box',halfExtents:[1.0,0.5,2.0]},blocking:true,pickable:true}}});
        var q = {x:0, y:Math.sin(.2), z:0, w:Math.cos(.2)};
        Engine.transform(body, {position:{x:-5,y:3,z:0}, rotation:q});
        var query = {shape:'box', halfExtents:{x:1,y:.5,z:2}, position:{x:-5,y:3,z:0}, rotation:q};
        var hit = Engine.physicsShapeSweep(query, {x:30,y:0,z:0}, q, {ignoreOwner:body,blockingOnly:true});
        assert(hit && hit.owner === wall && hit.normal.x < -.99, 'JS shape sweep failed');
        var moved = Engine.moveBody(body, {x:30,y:1,z:3});
        assert(moved.blocked && moved.contacts.length && moved.contacts[0].owner === wall, 'JS contact feedback missing');
        assert(moved.applied.x < 5 && moved.position.y > 3.99 && moved.position.z > 2.99, 'Rigid movement must not require a ground plane');
        query.position = moved.position;
        query.rotation = moved.rotation;
        assert(Engine.physicsShapeOverlap(query, {ignoreOwner:body,blockingOnly:true}).length === 0, 'JS movement penetrated');
        var pose = Engine.position(body);
        assert(Math.abs(pose.rotation.y - q.y) < .00001 && Math.abs(pose.yaw - .4) < .00001, 'Pose readback lost rotation or yaw compatibility');
        Engine.enabled(wall, false);
        q = {x:Math.sin(.35), y:0, z:0, w:Math.cos(.35)};
        var clear = Engine.moveBody(body, {x:0,y:0,z:0}, q);
        assert(!clear.blocked && Math.abs(clear.rotation.x - q.x) < .00001, 'JS pitch movement failed');
        var rejected = false;
        try { Engine.transform(body, {position:{x:99,y:99,z:99},rotation:{x:0,y:0,z:0,w:2}}); }
        catch (e) { rejected = true; }
        assert(rejected && Engine.position(body).x < 0, 'Invalid transform corrupted object');
    )JS");
    uint32_t body = world.registry().entities().back();
    auto full = glm::normalize(glm::angleAxis(.7f, vec3(1, 0, 0)) * glm::angleAxis(.5f, vec3(0, 0, 1)));
    world.animation.setAnimationJoints(body, {{{0, 1, 0}, quat(1, 0, 0, 0)}});
    auto attachment = world.animation.addAnimationCollider(body, 0, ColliderShape::box(vec3(.1f)),
                                                  {{0, .2f, 0}, quat(1, 0, 0, 0)}, false);
    world.render.setVisualPose(body, {.3f, .4f, .5f}, {1, 2, 1});
    auto before = world.snapshot({}, 0, 0, 0);
    world.transforms.setTransform(body, {{2, 3, 4}, full});
    auto after = world.snapshot({}, 1, 0, 0);
    const auto& object = world.get<Transform>(body).world;
    const auto& physical = world.physics().body(world.get<Collider>(body).body);
    check(sameRotation(physical.pose.rotation, full), "World transform must reach the physical body");
    const auto& proxy = after.proxies[world.get<Renderable>(body).slot].transform;
    check(sameRotation(proxy.rotation, full) && near(proxy.position, object.position + full * vec3(.3f, .4f, .5f)),
          "Render offset must follow all three axes of rotation");
    vec3 expected = object.position + full * (vec3(.3f, .4f, .5f) + world.get<Renderable>(body).appearance.scale * world.get<Renderable>(body).appearance.animationScale * vec3(0, 1, 0));
    check(near(vec3(transform(proxy) * vec4(0, 1, 0, 1)), expected),
          "GPU model matrix must include pitch and roll");
    check(before.proxies[world.get<Renderable>(body).slot].transform != proxy && !after.delta.moved.empty(),
          "Full rotation must publish a dirty transform and leave old snapshots immutable");
    auto attached = world.physics().body(attachment);
    check(near(attached.pose.position, object.position + full * vec3(0, 1.2f, 0)) &&
              sameRotation(attached.pose.rotation, full), "Joint colliders must follow full root rotation");
    auto document = ScenePersistence::capture(world, testAssets());
    auto encoded = document.json();
    check(encoded.at("version").uint() == 4, "Quaternion maps must declare their updated schema");
    auto decoded = SceneDocument::fromJson(Json::parse(encoded.dump()));
    check(sameRotation(decoded.entities.back().transform->local.rotation, full), "Quaternion scene data did not round-trip");
    // The migrated project and a saved Shader-backed map exercise disk IO.
    auto legacy = testAssets().load<SceneAsset>(AssetPath("/Game/Maps/RainCourt"));
    check(!legacy->scene.entities.empty(), "Migrated project map must remain loadable");
    auto directory = std::filesystem::path(AFTERLIGHT_ROOT) / "build" / ("kinematic-" + newPersistentId());
    AssetManager local{Project::create(directory, "Kinematic test")};
    registerEngineAssets(local);
    auto shader = world.resources.materials[0].shader;
    auto shaderHeader = shader->header();
    shaderHeader.storage = PayloadStorage::Inline;
    shaderHeader.source.clear();
    local.save(shader->reference().path, shaderHeader, shader->source);
    auto saved = ScenePersistence::save(world, local, AssetPath("/Game/Maps/Rotated"), "Rotated");
    local.scan();
    World fromDisk;
    ScenePersistence::load(fromDisk, local, saved.path);
    const auto& restoredObject = fromDisk.registry().entities().back();
    check(sameRotation(fromDisk.get<Transform>(restoredObject).world.rotation, full) &&
              sameRotation(fromDisk.physics().body(fromDisk.get<Collider>(restoredObject).body).pose.rotation, full),
          "Saved map must restore visual and physical rotation");
}
int main() {
    try {
        continuousMotion();
        rotationalMotion();
        transformsAndScript();
        std::cout << "PASS: oriented box/capsule CCD, rotational sweep, sliding, JS contacts, quaternion snapshots and scene persistence\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
