#include "physics/PhysicsScene.h"
#include "navigation/Navigation.h"
#include "core/World.h"
#include <future>
#include <iostream>
#include <stdexcept>
using namespace afterlight;
static void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
static BodyHandle box(PhysicsScene& scene, uint32_t owner, vec3 center, vec3 half, bool ground = false) {
    PhysicsBody b;
    b.owner = owner;
    b.pose.position = center;
    b.shape = ColliderShape::box(half);
    b.walkable = ground;
    return scene.create(b);
}
static QueryFilter blocking(uint32_t owner = 0) {
    QueryFilter f;
    f.blockingOnly = true;
    f.ignoreOwner = owner;
    f.mask = CollisionLayer::World | CollisionLayer::Character;
    return f;
}
int main() {
    try {
        PhysicsScene scene;
        box(scene, 1, {0, -.2f, 0}, {12, .2f, 10}, true);
        auto wall = box(scene, 2, {0, 1.5f, 0}, {.005f, 1.5f, 8});
        CapsuleQuery character{{-5, 1, 0}, .4f, 2};
        auto hit = scene.sweepCapsule(character, {30, 0, 0}, blocking());
        check(hit && hit->owner == 2 && hit->fraction < .17f && hit->normal.x < -.99f,
              "Continuous sweep must detect a 1 cm wall during a 30 m movement");
        auto move = scene.moveAndSlide(character, {30, 0, 3}, blocking());
        check(move.blocked && move.position.x < -.4f && move.position.z > 2.9f,
              "Wall contact must preserve tangential sliding");
        check(scene.overlapCapsule({move.position, .4f, 2}, blocking()).empty(),
              "Sweep produced penetration");
        check(!scene.sweepCapsule(character, {0, 0, 1}, blocking()),
              "Floor tangency must not block horizontal motion");
        auto originalRevision = scene.revision();
        scene.setEnabled(wall, false);
        check(scene.revision() > originalRevision && !scene.sweepCapsule(character, {10, 0, 0}, blocking()),
              "Disabled bodies must immediately disappear from spatial queries");
        scene.destroy(wall);
        auto replacement = box(scene, 3, {0, 4, 0}, {2, .2f, 2});
        check(replacement.slot == wall.slot && replacement.generation != wall.generation &&
                  !scene.contains(wall),
              "Reused slots must invalidate stale handles");
        bool rejected = false;
        try {
            scene.body(wall);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        check(rejected, "Stale physics handles must be rejected");
        check(Navigation::lineClear(scene, {-3, 0, 0}, {3, 0, 0}),
              "Raised geometry must not block a path by XZ alone");
        scene.setPose(replacement, {{0, 1.7f, 0}, quat(1, 0, 0, 0)});
        check(!Navigation::canStand(scene, {0, 0, 0}), "Standing agent must fail overhead clearance");
        check(Navigation::lineClear(scene, {-3, 0, 0}, {3, 0, 0}, {.4f, 1.3f, 0}),
              "Crouched agent must use its actual capsule height during path queries");
        PhysicsBody crouched;
        crouched.owner = 4;
        crouched.layer = CollisionLayer::Character;
        crouched.shape = ColliderShape::capsule(.4f, 1.3f);
        crouched.pose.position = {0, .65f, 0};
        auto body = scene.create(crouched);
        check(!scene.resizeCharacter(body, 2), "Standing up must be rejected under a low ceiling");
        check(std::abs(scene.body(body).pose.position.y - .65f) < 1e-5f,
              "Failed stance change moved the feet");
        scene.setPose(replacement, {{0, 4, 0}, quat(1, 0, 0, 0)});
        check(scene.resizeCharacter(body, 2) && std::abs(scene.body(body).pose.position.y - 1) < 1e-5f,
              "Standing in clear space must preserve foot height");
        scene.destroy(body);
        scene.destroy(replacement);

        auto rotated = box(scene, 5, {0, 1, 0}, {.02f, 1, 2});
        scene.setPose(rotated, {{0, 1, 0}, glm::angleAxis(Pi * .25f, vec3(0, 1, 0))});
        check(scene.sweepCapsule(character, {10, 0, 0}, blocking()).has_value(), "Sweep missed rotated OBB");
        auto rotatedRay = scene.raycast({-5, 1, 0}, {1, 0, 0}, 10);
        check(rotatedRay && rotatedRay->owner == 5 && std::abs(glm::length(rotatedRay->normal) - 1) < 1e-4f,
              "Raycast must return a world-space OBB normal");
        scene.destroy(rotated);
        PhysicsBody cap;
        cap.owner = 6;
        cap.shape = ColliderShape::capsule(.4f, 2);
        cap.pose.position = {0, 1, 0};
        auto capsule = scene.create(cap);
        check(!scene.raycast({.39f, 1.99f, -3}, {0, 0, 1}, 6),
              "Capsule ray test must reject empty corners inside its AABB");
        auto centerRay = scene.raycast({0, 1, -3}, {0, 0, 1}, 6);
        check(centerRay && std::abs(centerRay->distance - 2.6f) < .001f,
              "Capsule cylinder ray intersection is wrong");
        scene.setPose(capsule, {{0, 1, 0}, glm::angleAxis(Pi * .5f, vec3(0, 0, 1))});
        check(!scene.overlapCapsule({{.8f, 1, 0}, .15f, .3f}).empty(),
              "Queries missed an animated horizontal capsule");
        scene.destroy(capsule);

        PhysicsBody trigger;
        trigger.owner = 7;
        trigger.layer = CollisionLayer::Trigger;
        trigger.shape = ColliderShape::capsule(.5f, 1);
        trigger.pose.position = {0, 1, 0};
        trigger.blocking = false;
        auto sensor = scene.create(trigger);
        check(!scene.overlapCapsule({{0, 1, 0}, .2f, .4f}).empty(),
              "Trigger must be available to overlap queries");
        check(!scene.sweepCapsule(character, {10, 0, 0}, blocking()), "Trigger must not block locomotion");
        scene.destroy(sensor);
        auto thread = std::async(std::launch::async, [&] {
            try {
                scene.raycast({0, 1, 0}, {0, -1, 0}, 3);
            } catch (const std::logic_error&) {
                return true;
            }
            return false;
        });
        check(thread.get(), "PhysicsScene must enforce its thread owner");

        PhysicsScene gap;
        box(gap, 1, {-3, -.2f, 0}, {2, .2f, 3}, true);
        box(gap, 2, {3, -.2f, 0}, {2, .2f, 3}, true);
        check(!Navigation::ground(gap, {0, 0, 0}) && Navigation::findPath(gap, {-3, 0, 0}, {3, 0, 0}).empty(),
              "Navigation must derive missing ground from PhysicsScene, not an infinite Y=0 plane");

        World world;
        world.materials.push_back({});
        auto floor = world.spawn("ground", Shape::Box, {0, -.2f, 0}, {24, .4f, 20}, 0, true, false);
        world.configureCollider(floor, CollisionLayer::World, true, true, false);
        auto obstacle =
            world.spawn("independent collider", Shape::Box, {0, 1, 0}, {.1f, .1f, .1f}, 0, true, false);
        world.setColliderShape(obstacle, ColliderShape::box({.02f, 1, 3}));
        auto actor = world.spawn("actor", Shape::Capsule, {-3, 1, 0}, {1, 1, 1}, 0, true, false);
        world.playerId = actor;
        auto physicsRevision = world.physics().revision();
        world.setVisible(obstacle, false);
        world.setVisualPose(obstacle, {9, 9, 9}, {.01f, 5, 8});
        auto frame = world.snapshot({}, 1, 0, 0);
        auto slot = world.entity(obstacle).proxy;
        frame.proxies[slot].transform.position = {100, 100, 100};
        check(world.physics().revision() == physicsRevision && !frame.proxies[slot].attributes.visible,
              "Render presentation must not mutate physical scene data");
        auto stopped = world.rootMotion(actor, {8, 0, 0}, .2f);
        check(stopped.x < -.4f, "Root motion must collide with an invisible independently authored collider");
        world.setSolid(obstacle, false);
        check(world.moveCharacter(actor, {3, 0, 0}).x > 2,
              "Changing physical collision flags must immediately affect locomotion");
        world.setAnimationJoints(actor, {{{0, .2f, 0}, quat(1, 0, 0, 0)}});
        auto animated = world.addAnimationCollider(actor, 0, ColliderShape::capsule(.15f, .8f),
                                                   {{0, 0, 0}, quat(1, 0, 0, 0)}, false);
        auto bodyCount = world.physics().size();
        world.setAnimationJoints(actor, {{{0, 0, 1}, glm::angleAxis(Pi * .5f, vec3(0, 0, 1))}});
        const auto animatedBody = world.physics().body(animated);
        QueryFilter sensors;
        sensors.mask = CollisionLayer::Trigger;
        check(!world.physics().overlapCapsule({animatedBody.pose.position, .1f, .2f}, sensors).empty(),
              "Animation joint poses must feed actual queryable bodies");
        auto beforeRoot = animatedBody.pose.position;
        world.rootMotion(actor, {0, 0, 1}, 0);
        check(glm::distance(world.physics().body(animated).pose.position, beforeRoot) > .9f,
              "Animated attachments must follow collision-resolved root motion");
        world.setEnabled(actor, false);
        check(world.physics().bodies(sensors).empty(),
              "Disabled objects must disable all attached colliders");
        world.setEnabled(actor, true);
        check(world.physics().bodies(sensors).size() == 1, "Re-enabled object lost its animation collider");
        world.destroy(actor);
        check(world.physics().size() == bodyCount - 2 && !world.physics().contains(animated),
              "Destroying an object must release its primary and animation bodies");
        std::cout
            << "PASS: standalone physics queries, continuous sweep/slide, OBB/capsule normals, layers, "
               "stale handles, ownership, physical navigation/clearance, root motion, render isolation, "
               "animation colliders and lifetime\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
