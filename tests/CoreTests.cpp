#include "core/World.h"
#include "core/FrameMailbox.h"
#include "navigation/Navigation.h"
#include "scripting/ScriptRuntime.h"
#include <iostream>
#include <stdexcept>
#include <future>
using namespace afterlight;
static void check(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
int main() {
    try {
        World w;
        ScriptRuntime js(w);
        js.initialize();
        check(w.playerId != 0, "JS must spawn player");
        check(w.objects().size() > 20 && w.physics().size() == w.objects().size(),
              "Physical scene not populated");
        check(w.lights.size() >= 6, "RT lights not populated");
        auto route = Navigation::findPath(w.physics(), vec3(-8, 0, 4), vec3(-2, 0, 4), w.agent(w.playerId));
        check(!route.empty(), "A* must route around cargo");
        vec3 last(-8, 0, 4);
        for (auto p : route) {
            check(Navigation::lineClear(w.physics(), last, p, w.agent(w.playerId)),
                  "Smoothed segment clips obstacle");
            last = p;
        }
        check(Navigation::findPath(w.physics(), vec3(0), vec3(-4, 0, 4), w.agent(w.playerId)).empty(),
              "Blocked targets must fail");
        QueryFilter filter;
        filter.blockingOnly = true;
        filter.ignoreOwner = w.playerId;
        auto moved = w.physics().moveAndSlide({vec3(-8, 1, 4), .4f, 2}, vec3(9, 0, 0), filter);
        check(moved.position.x < -5, "Sweep must prevent tunneling");
        Input input;
        input.keys['W'] = true;
        auto initial = w.entity(w.playerId).position;
        for (int i = 0; i < 120; i++)
            js.tick(1.f / 60, input);
        check(glm::distance(initial, w.entity(w.playerId).position) > 2, "JS manual locomotion failed");
        input.keys.fill(false);
        for (int i = 0; i < 120; i++)
            js.tick(1.f / 60, input);
        check(w.state == "Idle", "Locomotion must settle at idle");
        input.keys[17] = true;
        js.tick(1.f / 60, input);
        check(std::abs(w.agent(w.playerId).height - 1.3f) < .001f && std::abs(w.feet(w.playerId).y) < .001f,
              "JS crouch must resize physical capsule and keep feet grounded");
        auto player = w.entity(w.playerId).position;
        auto ceiling = w.spawn("Stance test ceiling", Shape::Box, {player.x, 1.7f, player.z}, {2, .4f, 2}, 0,
                               true, false);
        input.keys[17] = false;
        js.tick(1.f / 60, input);
        check(w.agent(w.playerId).height < 1.4f && w.state == "Crouching",
              "JS must respect blocked standing clearance");
        w.destroy(ceiling);
        js.tick(1.f / 60, input);
        check(w.agent(w.playerId).height > 1.9f, "JS must stand once ceiling is removed");
        js.execute(R"JS(
            (function() {
                function check(ok, message) { if (!ok) throw new Error(message); }
                var revision = Engine.physicsRevision();
                var id = Engine.spawn('Binding test', false, 30, 1, 0, 1, 2, 1, 0, true, false);
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
        ScriptRuntime gameplay(interactionWorld);
        gameplay.initialize();
        Input click;
        gameplay.tick(1.f / 60, click);
        auto screenPoint = [&](vec3 point) {
            auto clip = interactionWorld.camera.projection(float(click.width) / click.height) *
                        interactionWorld.camera.view() * vec4(point, 1);
            click.mouseX = (clip.x / clip.w * .5f + .5f) * click.width;
            click.mouseY = (clip.y / clip.w * .5f + .5f) * click.height;
        };
        // Exercise the same mouse-pick -> JS command -> A* -> facing -> interaction path as the window.
        uint32_t console = 0, door = 0;
        for (const auto& e : interactionWorld.objects()) {
            if (e.name == "Power console")
                console = e.id;
            if (e.name == "Service door")
                door = e.id;
        }
        screenPoint(interactionWorld.entity(console).position);
        check(interactionWorld.pick(click.mouseX, click.mouseY, click) == console,
              "Interactive object picking failed");
        click.leftPressed = true;
        gameplay.tick(1.f / 60, click);
        click.leftPressed = false;
        for (int i = 0; i < 1500; i++) {
            gameplay.tick(1.f / 60, click);
            check(Navigation::canStand(interactionWorld.physics(),
                                       interactionWorld.feet(interactionWorld.playerId),
                                       interactionWorld.agent(interactionWorld.playerId)),
                  "Click locomotion penetrated obstacle");
        }
        check(!interactionWorld.physics().body(interactionWorld.entity(door).physical).blocking &&
                  interactionWorld.entity(door).position.y > 4,
              "Approach-and-use must open door and update collision");
        check(glm::distance(vec2(interactionWorld.entity(interactionWorld.playerId).position.x,
                                 interactionWorld.entity(interactionWorld.playerId).position.z),
                            vec2(-6, -1.7f)) < .25f,
              "Navigation must arrive precisely");
        click.pressed[9] = true;
        gameplay.tick(1.f / 60, click);
        click.pressed.fill(false);
        auto stopped = interactionWorld.entity(interactionWorld.playerId).position;
        screenPoint(vec3(6, 0, 4));
        click.leftPressed = true;
        gameplay.tick(1.f / 60, click);
        click.leftPressed = false;
        for (int i = 0; i < 60; i++)
            gameplay.tick(1.f / 60, click);
        check(glm::distance(stopped, interactionWorld.entity(interactionWorld.playerId).position) < .1f,
              "Deselected character must ignore commands");
        FrameMailbox box;
        Frame a;
        a.tick = 1;
        box.publish(a);
        a.tick = 2;
        box.publish(a);
        Frame received;
        check(box.consume(received) && received.tick == 2, "Mailbox must consume latest snapshot");
        box.close();
        check(!box.consume(received), "Closed mailbox must wake and exit");
        FrameMailbox waiting;
        auto task = std::async(std::launch::async, [&] {
            Frame f;
            return waiting.consume(f);
        });
        waiting.close();
        check(!task.get(), "Closing empty mailbox must unblock consumer");
        std::cout << "PASS: JS scene/locomotion, A* clearance, blocked goal, swept collision, camera ray, "
                     "click-to-interact/facing/door collision, deselection, bounded mailbox and shutdown\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
