#include "TestEntities.h"
#include "TestProject.h"
#include "core/World.h"
#include "core/FrameMailbox.h"
#include "navigation/Navigation.h"
#include "scripting/RuntimeHost.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <future>
using namespace afterlight;
static void check(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
static void tickAnimated(RuntimeHost& scripts, World& world, const Input& input) {
    float yaw = world.get<Transform>(world.gameplay.playerId).yaw();
    scripts.tick(1.f / 60, input);
    auto direction = world.animation.animationOutput(world.gameplay.playerId).rootMotion.rotation * vec3(0, 0, 1);
    check(std::abs(std::remainder(world.get<Transform>(world.gameplay.playerId).yaw() - yaw - std::atan2(direction.x, direction.z), 2 * Pi)) < 1e-5f,
          "Player movement and interaction must consume animated root rotation without scripted yaw");
}
int main() {
    try {
        World w;
        RuntimeHost js(w, testAssets());
        js.initialize(testProject());
        check(w.gameplay.playerId != 0, "JS must spawn player");
        check(w.registry().entities().size() > 20 && w.physics().size() == w.registry().view<Collider>().size(),
              "Physical scene not populated");
        check(w.registry().view<LightComponent>().size() >= 6, "RT lights not populated");
        auto route = Navigation::findPath(w.physics(), vec3(-8, 0, 4), vec3(-2, 0, 4), w.motion.agent(w.gameplay.playerId));
        check(!route.empty(), "A* must route around cargo");
        vec3 last(-8, 0, 4);
        for (auto p : route) {
            check(Navigation::lineClear(w.physics(), last, p, w.motion.agent(w.gameplay.playerId)),
                  "Smoothed segment clips obstacle");
            last = p;
        }
        check(Navigation::findPath(w.physics(), vec3(0), vec3(-4, 0, 4), w.motion.agent(w.gameplay.playerId)).empty(),
              "Blocked targets must fail");
        QueryFilter filter;
        filter.blockingOnly = true;
        filter.ignoreOwner = w.gameplay.playerId;
        auto moved = w.physics().moveAndSlide({vec3(-8, 1, 4), .4f, 2}, vec3(9, 0, 0), filter);
        check(moved.position.x < -5, "Sweep must prevent tunneling");
        Input input;
        input.keys['W'] = true;
        auto initial = w.get<Transform>(w.gameplay.playerId).world.position;
        for (int i = 0; i < 120; i++)
            tickAnimated(js, w, input);
        check(glm::distance(initial, w.get<Transform>(w.gameplay.playerId).world.position) > 2, "JS manual locomotion failed");
        input.keys.fill(false);
        input.keys['D'] = true;
        float turnStart = w.get<Transform>(w.gameplay.playerId).yaw(), previousTurn = 0, maxTurnChange = 0;
        for (int i = 0; i < 90; ++i) {
            float yaw = w.get<Transform>(w.gameplay.playerId).yaw();
            tickAnimated(js, w, input);
            float turn = std::remainder(w.get<Transform>(w.gameplay.playerId).yaw() - yaw, 2 * Pi);
            maxTurnChange = std::max(maxTurnChange, std::abs(turn - previousTurn));
            previousTurn = turn;
        }
        check(std::abs(w.get<Transform>(w.gameplay.playerId).yaw() - turnStart) > .5f,
              "A manual direction change must turn the animated player");
        check(maxTurnChange < glm::radians(4.f), "Player turn rate must not snap to the scripted turn limit");
        input.keys.fill(false);
        for (int i = 0; i < 120; i++)
            tickAnimated(js, w, input);
        check(w.gameplay.state == "Idle", "Locomotion must settle at idle");
        input.keys[17] = true;
        tickAnimated(js, w, input);
        check(std::abs(w.motion.agent(w.gameplay.playerId).height - 1.3f) < .001f && std::abs(w.motion.feet(w.gameplay.playerId).y) < .001f,
              "JS crouch must resize physical capsule and keep feet grounded");
        auto player = w.get<Transform>(w.gameplay.playerId).world.position;
        auto ceiling = spawnTest(w, "Stance test ceiling", Shape::Box, {player.x, 1.7f, player.z}, {2, .4f, 2}, 0,
                               true, false);
        input.keys[17] = false;
        tickAnimated(js, w, input);
        check(w.motion.agent(w.gameplay.playerId).height < 1.4f && w.gameplay.state == "Crouching",
              "JS must respect blocked standing clearance");
        w.destroy(ceiling);
        tickAnimated(js, w, input);
        check(w.motion.agent(w.gameplay.playerId).height > 1.9f, "JS must stand once ceiling is removed");
        js.execute(R"JS(
            (function() {
                function check(ok, message) { if (!ok) throw new Error(message); }
                var revision = Engine.physicsRevision();
                var id = Engine.create({name:'Binding test',components:{transform:{position:[30,1,0]},render:{scale:[1,2,1],material:0},collider:{shape:{type:'box',halfExtents:[0.5,1.0,0.5]},blocking:true,pickable:true}}});
                Engine.collider(id, {shape:'box', halfExtents:{x:.2,y:1,z:.2}, blocking:true, layer:1});
                Engine.visible(id, false);
                var hit = Engine.physicsRaycast({x:28,y:1,z:0}, {x:1,y:0,z:0}, 6, 1);
                check(hit && hit.owner === id && Math.abs(hit.distance - 1.8) < .001, 'JS physical ray');
                hit = Engine.physicsSweep({x:28,y:1,z:0}, .2, 1, {x:4,y:0,z:0}, 1, 0);
                check(hit && hit.owner === id && hit.fraction < .41, 'JS physical sweep');
                var overlaps = Engine.physicsOverlap({x:30,y:1,z:0}, .2, 1, 1, 0);
                check(overlaps.length === 1 && overlaps[0].owner === id, 'JS physical overlap');
                Engine.animationJoints(id, [{position:{x:0,y:0,z:0}}]);
                Engine.animationCollider(id, 0, {shape:'capsule',radius:.15,height:.6});
                Engine.animationJoints(id, [{position:{x:0,y:0,z:2},rotation:{x:0,y:0,z:.70710678,w:.70710678}}]);
                check(Engine.physicsOverlap({x:30,y:1,z:2}, .1, .2, 4, 0).length === 1, 'JS animated collider');
                Engine.enabled(id, false);
                check(Engine.physicsOverlap({x:30,y:1,z:2}, .1, .2, 4, 0).length === 0, 'JS disable attachment');
                Engine.enabled(id, true);
                Engine.destroy(id);
                check(Engine.physicsOverlap({x:30,y:1,z:2}, .1, .2, 4, 0).length === 0, 'JS destroy attachment');
                check(Engine.physicsRevision() > revision, 'JS revision');
                var p = Engine.position(Locomotion.id);
                var moved = Engine.rootMotion(Locomotion.id, {x:0,y:0,z:0}, 0);
                check(Math.abs(p.x-moved.x) < .001 && Math.abs(p.z-moved.z) < .001, 'JS root motion');
            })();
        )JS",
                   "physics-bindings-test");
        auto center = w.groundAt(640, 400, input);
        check(center && std::isfinite(center->x) && std::isfinite(center->z),
              "Physical ground picking failed");
        World interactionWorld;
        RuntimeHost gameplay(interactionWorld, testAssets());
        gameplay.initialize(testProject());
        Input click;
        tickAnimated(gameplay, interactionWorld, click);
        auto screenPoint = [&](vec3 point) {
            auto clip = interactionWorld.resources.camera.projection(float(click.width) / click.height) *
                        interactionWorld.resources.camera.view() * vec4(point, 1);
            click.mouseX = (clip.x / clip.w * .5f + .5f) * click.width;
            click.mouseY = (clip.y / clip.w * .5f + .5f) * click.height;
        };
        // Exercise the same mouse-pick -> JS command -> A* -> facing -> interaction path as the window.
        uint32_t console = 0, door = 0;
        for (const auto& e : interactionWorld.registry().entities()) {
            if (interactionWorld.get<Identity>(e).name == "Power console")
                console = e;
            if (interactionWorld.get<Identity>(e).name == "Service door")
                door = e;
        }
        screenPoint(interactionWorld.get<Transform>(console).world.position);
        check(interactionWorld.pick(click.mouseX, click.mouseY, click) == console,
              "Interactive object picking failed");
        click.leftPressed = true;
        tickAnimated(gameplay, interactionWorld, click);
        click.leftPressed = false;
        for (int i = 0; i < 1500; i++) {
            tickAnimated(gameplay, interactionWorld, click);
            check(Navigation::canStand(interactionWorld.physics(),
                                       interactionWorld.motion.feet(interactionWorld.gameplay.playerId),
                                       interactionWorld.motion.agent(interactionWorld.gameplay.playerId)),
                  "Click locomotion penetrated obstacle");
        }
        check(!interactionWorld.physics().body(interactionWorld.get<Collider>(door).body).blocking &&
                  interactionWorld.get<Transform>(door).world.position.y > 4,
              "Approach-and-use must open door and update collision");
        check(glm::distance(vec2(interactionWorld.get<Transform>(interactionWorld.gameplay.playerId).world.position.x,
                                 interactionWorld.get<Transform>(interactionWorld.gameplay.playerId).world.position.z),
                            vec2(-6, -1.7f)) < .25f,
              "Navigation must arrive precisely");
        click.pressed[9] = true;
        tickAnimated(gameplay, interactionWorld, click);
        click.pressed.fill(false);
        auto stopped = interactionWorld.get<Transform>(interactionWorld.gameplay.playerId).world.position;
        screenPoint(vec3(6, 0, 4));
        click.leftPressed = true;
        tickAnimated(gameplay, interactionWorld, click);
        click.leftPressed = false;
        for (int i = 0; i < 60; i++)
            tickAnimated(gameplay, interactionWorld, click);
        check(glm::distance(stopped, interactionWorld.get<Transform>(interactionWorld.gameplay.playerId).world.position) < .1f,
              "Deselected character must ignore commands");
        // Block an already accepted route. Replanning must use physical feedback from
        // root motion, rather than mistaking the requested velocity for actual travel.
        gameplay.execute("Locomotion.command({x:-8,y:0,z:-1.7});");
        check(!interactionWorld.gameplay.path.empty(), "Dynamic obstacle test requires an initially valid route");
        auto blocker = spawnTest(interactionWorld, "New route obstruction", Shape::Box, {-8, 1, -1.7f}, {1, 2, 1},
                                              0, true, false);
        for (int i = 0; i < 600; ++i) {
            tickAnimated(gameplay, interactionWorld, click);
            check(Navigation::canStand(interactionWorld.physics(),
                                       interactionWorld.motion.feet(interactionWorld.gameplay.playerId),
                                       interactionWorld.motion.agent(interactionWorld.gameplay.playerId)),
                  "Animated root motion must not cross a new route obstruction");
        }
        check(interactionWorld.gameplay.path.empty() && interactionWorld.gameplay.message == "Route is no longer reachable",
              "Blocked animated travel must replan and cancel an unreachable route");
        interactionWorld.destroy(blocker);
        // The render scene is persistent: slots outlive the objects that move through
        // them, and a change is reported as an event rather than rediscovered by
        // comparing whole frames.
        RenderScene scene;
        ProxyTransform pose;
        pose.position = {1, 2, 3};
        ProxyAttributes look;
        look.entity = 7;
        auto first = scene.create(pose, look);
        auto second = scene.create(pose, look);
        auto opening = scene.publish();
        check(opening.structural.size() == 2 && opening.topology && opening.moved.empty(),
              "Creating proxies must be structural and must grow the slot capacity");
        pose.position = {4, 2, 3};
        scene.setTransform(first, pose);
        auto motion = scene.publish();
        check(motion.moved.size() == 1 && motion.moved[0] == first && motion.attributes.empty() &&
                  motion.structural.empty() && motion.topology == opening.topology,
              "Moving a proxy must take the transform path and nothing else");
        check(motion.base == opening.revision, "Deltas must chain so a consumer can detect a gap");
        scene.setTransform(first, pose); // Same value as it already holds.
        auto idle = scene.publish();
        check(idle.empty() && idle.base == idle.revision,
              "A scene that did not actually change must produce no events at all");
        scene.setMaterial(second, 3);
        auto repaint = scene.publish();
        check(repaint.attributes.size() == 1 && repaint.attributes[0] == second && repaint.moved.empty() &&
                  repaint.topology == opening.topology,
              "A material change must not be mistaken for a transform or a topology change");
        scene.destroy(first);
        auto removal = scene.publish();
        check(removal.structural.size() == 1 && !scene.proxy(first).live &&
                  !scene.proxy(first).attributes.visible && scene.proxy(second).attributes.entity == 7,
              "Destroying a proxy must free its slot without renumbering any other");
        check(scene.create(pose, look) == first && scene.capacity() == 2,
              "A freed slot must be reused rather than growing the scene");
        auto settled = scene.publish();
        check(settled.topology == opening.topology,
              "Reusing a slot for the same geometry must not force a rebuild");
        ProxyAttributes capsule = look;
        capsule.shape = Shape::Capsule;
        scene.destroy(first);
        scene.create(pose, capsule);
        check(scene.publish().topology != settled.topology,
              "Rebinding a slot to different geometry must be a topology change");
        // Topology is reported as a running count rather than a per-delta flag, so a
        // consumer that skipped the snapshot carrying the change still sees it. As a flag
        // it would have been swallowed with that snapshot, leaving the consumer refitting
        // an acceleration structure whose topology had moved out from under it.
        auto skipped = scene.publish();
        scene.create(pose, capsule);
        scene.publish(); // Never delivered.
        pose.position = {7, 2, 3};
        scene.setTransform(second, pose);
        check(scene.publish().topology != skipped.topology,
              "A topology change must survive a snapshot the consumer never read");
        // A consumer mirrors the scene by slot, so growth alone must never force it to
        // rewrite the slots it already holds. That is only safe because every slot beyond
        // its previous capacity arrives as a structural event; if creation ever stopped
        // reporting itself, the consumer would draw whatever was left in those slots.
        const auto known = scene.capacity();
        for (int i = 0; i < 3; i++)
            scene.create(pose, look);
        pose.position = {9, 2, 3}; // Growth and movement in one delta, as a busy frame sees it.
        scene.setTransform(second, pose);
        const auto grown = scene.publish();
        check(grown.moved.size() == 1, "Growth must not disturb the events reported for existing slots");
        for (uint32_t slot = known; slot < scene.capacity(); slot++)
            check(std::find(grown.structural.begin(), grown.structural.end(), slot) != grown.structural.end(),
                  "Every newly created slot must be announced so growth stays incremental");
        // The world has to publish those events on the caller's behalf; forgetting to
        // would leave the renderer showing stale geometry with no way to notice.
        World tracked;
        RuntimeHost trackedJs(tracked, testAssets());
        trackedJs.initialize(testProject());
        auto opened = tracked.snapshot({}, 1, 0, 0);
        check(opened.proxies.size() == tracked.registry().view<Renderable>().size() &&
                  opened.delta.structural.size() == opened.proxies.size(),
              "The first snapshot must present every proxy as newly created");
        check(tracked.snapshot({}, 2, 0, 0).delta.empty(),
              "Snapshotting a world nobody touched must carry no events");
        auto slot = tracked.get<Renderable>(tracked.gameplay.playerId).slot;
        tracked.transforms.setTransform(tracked.gameplay.playerId, {{5, 1, 5}, glm::angleAxis(.5f, vec3(0, 1, 0))});
        tracked.render.setScale(tracked.gameplay.playerId, {1, 2, 1});
        auto walked = tracked.snapshot({}, 3, 0, 0);
        check(walked.delta.moved.size() == 1 && walked.delta.moved[0] == slot &&
                  walked.delta.attributes.empty() && walked.delta.structural.empty(),
              "Moving one object must dirty exactly one slot, and only its transform");
        check(walked.proxies[slot].transform.position.x == 5.f,
              "Events name slots; the values must come from the proxy array");
        tracked.render.setVisible(tracked.gameplay.playerId, false);
        auto hidden = tracked.snapshot({}, 4, 0, 0);
        check(hidden.delta.attributes.size() == 1 && hidden.delta.moved.empty() &&
                  !hidden.proxies[slot].attributes.visible,
              "Hiding an object must take the attribute path, not rebuild the proxy");
        FrameMailbox box;
        Frame a;
        a.tick = 1;
        box.publish(a);
        a.tick = 2;
        box.publish(a);
        FrameRef received;
        uint64_t seen = 0;
        check(box.acquire(received, seen) == FrameStatus::Fresh && received->tick == 2,
              "Mailbox must deliver the latest snapshot and drop the unread one");
        // The renderer must be able to outrun the simulation: acquiring again reports that
        // nothing is new while leaving the caller's snapshot intact to re-present.
        check(box.acquire(received, seen) == FrameStatus::Repeat && received && received->tick == 2,
              "Acquire without a new publish must repeat, not block or clear");
        a.tick = 3;
        box.publish(a);
        check(box.acquire(received, seen) == FrameStatus::Fresh && received->tick == 3,
              "A later publish must be observed as fresh");
        // Dropping a snapshot must cost its latency, not its events. The renderer mirrors
        // the scene by applying deltas in order, so a gap in the chain forces it to
        // rewrite every slot; the mailbox therefore folds an unread delta into its
        // replacement and leaves the chain continuous.
        Frame dropped;
        dropped.delta.base = 10;
        dropped.delta.revision = 11;
        dropped.delta.moved = {4};
        dropped.delta.structural = {7};
        box.publish(dropped);
        Frame replacement;
        replacement.delta.base = 11;
        replacement.delta.revision = 12;
        replacement.delta.moved = {4, 5};
        box.publish(replacement); // Replaces a snapshot the consumer never took.
        check(box.acquire(received, seen) == FrameStatus::Fresh && received->delta.base == 10 &&
                  received->delta.revision == 12,
              "A dropped delta must extend the chain back to where the consumer still sits");
        check(received->delta.moved == std::vector<uint32_t>{4, 5} &&
                  received->delta.structural == std::vector<uint32_t>{7},
              "Folded events must be the union of both deltas, counted once");
        Frame following;
        following.delta.base = 12;
        following.delta.revision = 13;
        following.delta.moved = {9};
        box.publish(following);
        check(box.acquire(received, seen) == FrameStatus::Fresh && received->delta.base == 12 &&
                  received->delta.moved == std::vector<uint32_t>{9},
              "A delivered snapshot must not be folded into the next one");
        box.close();
        check(box.acquire(received, seen) == FrameStatus::Closed, "Closed mailbox must wake and exit");
        FrameMailbox waiting;
        auto task = std::async(std::launch::async, [&] {
            FrameRef f;
            uint64_t cursor = 0;
            return waiting.acquire(f, cursor);
        });
        waiting.close();
        check(task.get() == FrameStatus::Closed, "Closing empty mailbox must unblock consumer");
        std::cout << "PASS: JS scene/locomotion, A* clearance, blocked goal, swept collision, camera ray, "
                     "click-to-interact/facing/door collision, deselection, decoupled mailbox and "
                     "shutdown\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
