#include "TestProject.h"
#include "scripting/ScriptRuntime.h"
#include "animation/SkinnedMesh.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
using namespace afterlight;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        World world;
        ScriptRuntime scripts(world, testAssets());
        scripts.initialize();
        Input input;
        uint32_t dog = 0;
        for (const auto& o : world.registry().entities())
            if (world.get<Identity>(o).name == "Ash")
                dog = o;
        check(dog != 0, "Scene must spawn a companion dog");
        auto initial = world.get<Transform>(dog).world.position;
        scripts.execute("Locomotion.command({x:-8,y:0,z:4});");
        float closest = 100;
        std::ofstream trace;
        if (argc == 2) {
            trace.open(argv[1]);
            trace << "frame,x,z,yaw,rootYaw,bodyYaw\n";
        }
        float previousTurn = 0, maxTurnChange = 0, previousBodyTurn = 0, maxBodyTurnChange = 0;
        vec3 previousBodyForward(0);
        auto tick = [&](int frame) {
            float beforeYaw = world.get<Transform>(dog).yaw();
            scripts.tick(1.f / 60, input);
            const auto& object = world.get<Transform>(dog);
            float turn = std::remainder(object.yaw() - beforeYaw, 2 * Pi);
            maxTurnChange = std::max(maxTurnChange, std::abs(turn - previousTurn));
            previousTurn = turn;
            auto forward = world.animation.animationOutput(dog).rootMotion.rotation * vec3(0, 0, 1);
            float rootTurn = std::atan2(forward.x, forward.z);
            check(std::abs(turn - rootTurn) < 1e-5f,
                  "Following must apply the solver's root turn, without a separate scripted yaw");
            auto bodyForward =
                object.world.rotation * world.get<JointPose>(dog).model[0].rotation * vec3(0, 0, 1);
            if (frame > 0) {
                float bodyTurn =
                    std::atan2(glm::cross(previousBodyForward, bodyForward).y,
                               previousBodyForward.x * bodyForward.x + previousBodyForward.z * bodyForward.z);
                if (frame > 1)
                    maxBodyTurnChange = std::max(maxBodyTurnChange, std::abs(bodyTurn - previousBodyTurn));
                previousBodyTurn = bodyTurn;
            }
            previousBodyForward = bodyForward;
            if (trace)
                trace << frame << ',' << object.world.position.x << ',' << object.world.position.z << ',' << object.yaw()
                      << ',' << rootTurn << ',' << std::atan2(bodyForward.x, bodyForward.z) << '\n';
        };
        for (int i = 0; i < 540; ++i) {
            tick(i);
            auto p = world.get<Transform>(dog).world.position;
            check(Navigation::canStand(world.physics(), world.motion.feet(dog), world.motion.agent(dog), world.resources.navigation),
                  "Dog must remain outside obstacles");
            float distance = glm::length(vec2(p.x - world.get<Transform>(world.gameplay.playerId).world.position.x,
                                              p.z - world.get<Transform>(world.gameplay.playerId).world.position.z));
            if (i > 420)
                closest = std::min(closest, distance);
            check(distance > .6f, "Dog must not overlap the player capsule");
        }
        check(glm::distance(initial, world.get<Transform>(dog).world.position) > 3, "Dog must follow moving player");
        check(closest < 2.5f, "Dog must catch up and stop near player");
        auto frame = world.snapshot(input, 540, 9, 0);
        check(frame.skins.size() == 2, "Player and dog must use actual skinned meshes");
        for (const auto& skin : frame.skins) {
            auto vertices = deformSkin(*skin.mesh, skin.palette);
            check(vertices.size() > 500, "Scene must use imported character geometry");
            for (const auto& v : vertices)
                check(std::isfinite(v.position.x + v.position.y + v.position.z),
                      "Skin deformation must stay finite");
        }
        auto before = frame.skins[0].palette;
        scripts.execute("Locomotion.command({x:-7,y:0,z:1});");
        for (int i = 0; i < 45; ++i)
            tick(540 + i);
        check(before != world.snapshot(input, 585, 9.75, 0).skins[0].palette,
              "Skin palette must follow animation and movement");
        check(maxTurnChange < glm::radians(3.f), "Repath must not produce a step in the dog's turn rate");
        // The animated leader changes speed through its stride; allow that extra turn
        // response, while still rejecting the former 7.45-degree scripted repath steps.
        check(maxBodyTurnChange < glm::radians(4.f), "Repath must not jerk the dog's body skeleton");
        std::cout << "Maximum root/body turn increment change: " << glm::degrees(maxTurnChange) << "/"
                  << glm::degrees(maxBodyTurnChange) << " degrees/frame; closest: " << closest << '\n';
        std::cout
            << "PASS: skinned biped, dog obstacle navigation, following distance and animated palettes\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
