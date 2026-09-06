#include "animation/Animation.h"
#include "animation/ai4animation/Controller.h"
#include "core/World.h"
#include "scripting/ScriptRuntime.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace afterlight;
namespace anim = afterlight::animation;
namespace ai = anim::ai4animation;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
static void close(float a, float b, const char* message, float eps = .0002f) {
    check(std::abs(a - b) < eps, message);
}
static void golden(const std::filesystem::path& directory, const char* name) {
    ai::OnnxModel model(directory / (std::string(name) + ".onnx"));
    std::ifstream file(directory / (std::string(name) + ".golden"), std::ios::binary);
    auto read = [&](void* data, size_t n) {
        check(bool(file.read(static_cast<char*>(data), std::streamsize(n))),
              "Missing/truncated PyTorch golden fixture");
    };
    uint32_t count, inputs, outputs;
    read(&count, 4);
    read(&inputs, 4);
    read(&outputs, 4);
    std::vector<float> x(inputs), expected(outputs);
    float maxError = 0;
    for (uint32_t i = 0; i < count; ++i) {
        read(x.data(), inputs * 4);
        read(expected.data(), outputs * 4);
        auto actual = model.run(x);
        check(actual.size() == expected.size(), "ONNX output shape mismatch");
        for (size_t j = 0; j < actual.size(); ++j) {
            float error = std::abs(actual[j] - expected[j]);
            maxError = std::max(error, maxError);
            check(error < .0002f + .0002f * std::abs(expected[j]),
                  "Native ONNX differs from original PyTorch");
        }
    }
    std::cout << directory.filename().string() << "/" << name << " max error: " << maxError << "\n";
}
// Independent peer demonstrates that World and Instance contain no neural-model assumptions.
class ConstantSolver final : public anim::Solver {
    float speed_;

  public:
    explicit ConstantSolver(float speed) : speed_(speed) {}
    void reset(const anim::Context&) override {}
    void evaluate(const anim::Context& c, anim::Output& out) override {
        out.localPose = c.skeleton.restPose();
        out.rootMotion.position = {speed_ * c.dt, 0, 0};
        out.events.push_back({"step", speed_});
    }
};
static void framework() {
    std::vector<anim::Joint> joints = {
        {"root", -1, {}}, {"middle", 0, {{0, 1, 0}, {1, 0, 0, 0}}}, {"tip", 1, {{0, 1, 0}, {1, 0, 0, 0}}}};
    auto skeleton = std::make_shared<anim::Skeleton>(joints);
    auto local = skeleton->restPose();
    local[0].rotation = glm::angleAxis(Pi / 2, vec3(0, 0, 1));
    auto model = skeleton->toModel(local);
    close(model[2].position.x, -2, "FK must compose parent rotations");
    auto roundtrip = skeleton->toLocal(model);
    close(roundtrip[2].position.y, 1, "Model/local round trip");
    model = skeleton->toModel(skeleton->restPose());
    vec3 pole(0, 0, 1);
    anim::solveFabrik(*skeleton, model, {0, 1, 2}, {1, 1, 0}, quat(1, 0, 0, 0), 20, .001f, &pole);
    check(glm::distance(model[2].position, vec3(1, 1, 0)) < .002f, "FABRIK must reach target");
    close(glm::distance(model[0].position, model[1].position), 1, "IK must preserve first segment", .002f);
    close(glm::distance(model[1].position, model[2].position), 1, "IK must preserve second segment", .002f);
    bool rejected = false;
    try {
        anim::Skeleton bad({{"cycle", 0, {}}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "Skeleton must reject cyclic topology");
    anim::Instance instance(skeleton, std::make_unique<ConstantSolver>(1.f));
    close(instance.evaluate(.1f, {}, {}).rootMotion.position.x, .1f, "Generic solver output");
    instance.setSolver(std::make_unique<ConstantSolver>(2.f));
    close(instance.evaluate(.1f, {}, {}).rootMotion.position.x, .2f,
          "Solver swap must preserve generic interface");
    World world;
    world.materials.emplace_back();
    world.spawn("ground", Shape::Box, {0, -.25f, 0}, {20, .5f, 20}, 0, true, false);
    world.configureCollider(1, CollisionLayer::World, true, true, false);
    auto owner = world.spawn("actor", Shape::Capsule, {0, 1, 0}, {1, 1, 1}, 0, false, false);
    world.attachAnimation(owner, skeleton, std::make_unique<ConstantSolver>(1.f));
    world.updateAnimations(.1f);
    close(world.entity(owner).position.x, .1f, "Animation root motion reaches physics");
    auto collider = world.addAnimationCollider(owner, 2, ColliderShape::box(vec3(.1f)), {}, false);
    world.updateAnimations(.1f);
    close(world.physics().body(collider).pose.position.x, .2f, "Animated colliders follow root");
    close(world.physics().body(collider).pose.position.y, 3, "Animated colliders consume model-space FK");
    world.setEnabled(owner, false);
    world.updateAnimations(.1f);
    close(world.entity(owner).position.x, .2f, "Disabled animation must not advance");
    world.setEnabled(owner, true);
    world.setAnimationSolver(owner, std::make_unique<ConstantSolver>(2.f));
    world.updateAnimations(.1f);
    close(world.entity(owner).position.x, .4f, "World supports peer solver replacement");
    world.destroy(owner);
    world.updateAnimations(.1f);
    check(!world.physics().contains(collider), "Destroy releases animation colliders");
}
static void scriptIntegration() {
    World world;
    ScriptRuntime script(world);
    script.initialize();
    auto id = world.playerId;
    script.execute("Engine.animation(" + std::to_string(id) +
                   ", 'animations/ai4animation/biped/controller.a4c', {rootMotion:false,rootOffset:{y:-1}});"
                   "Engine.animationInput(" +
                   std::to_string(id) + ", {action:'Idle'});");
    Input input;
    script.tick(1.f / 60, input);
    check(world.entity(id).joints.size() == 23, "JS tick must evaluate attached animation");
    auto frame = world.snapshot(input, 1, 0, 0, true);
    check(frame.skeletons.size() == 2 && frame.skeletons[0].jointWorld.size() == 23,
          "Snapshot publishes immutable skeletal transforms");
    auto saved = frame.skeletons[0].jointWorld[0];
    auto collider = world.addAnimationCollider(id, 3, ColliderShape::box(vec3(.1f)), {}, false);
    script.execute("Engine.animationInput(" + std::to_string(id) +
                   ", {action:'Locomotion',velocity:{z:1},"
                   "trajectory:[{time:0,position:{z:0}},{time:0.5,position:{z:0.5},velocity:{z:1}}]});");
    for (int i = 0; i < 10; ++i)
        script.tick(1.f / 60, input);
    check(frame.skeletons[0].jointWorld[0] == saved,
          "World updates must not mutate published skeleton snapshot");
    script.execute("Engine.animationReset(" + std::to_string(id) + ");Engine.animationDetach(" +
                   std::to_string(id) + ");");
    check(world.snapshot(input, 2, 0, 0).skeletons.size() == 1,
          "Detach removes player pose but keeps companion");
    check(world.entity(id).joints.empty() && !world.physics().contains(collider),
          "Detach releases skeleton-dependent colliders so another rig can be attached");
}
static void sequence() {
    // Layout is structure-of-arrays PER FRAME; yaw is degrees, and deltas accumulate in the original root
    // frame.
    std::vector<float> y(3 * 15, 0);
    for (size_t f = 0; f < 3; ++f) {
        y[f * 15 + 8] = 1;
        y[f * 15 + 10] = 1;
        y[f * 15 + 4] = 2;
    }
    y[15] = 1;
    y[16] = 90;
    y[30] = 1;
    auto s = ai::decodeSequence(y, 1, 3, 1, {});
    close(s.frames[2].root.position.x, 2, "Root deltas must not rotate before accumulation");
    close((s.frames[1].root.rotation * vec3(0, 0, 1)).x, 1, "Root yaw ABI is degrees");
    close(s.sample(.25f).root.position.x, .5f, "Sequence interpolation");
    s.age = 10;
    close(s.sample(0).root.position.x, 2, "Sequence end clamps safely");
    s.rebase({}, {{3, 0, 0}, quat(1, 0, 0, 0)});
    close(s.frames[2].root.position.x, 5, "Prediction physics feedback rebasing");
}
static void controller(const std::filesystem::path& directory) {
    golden(directory, "network");
    golden(directory, "postprocessor");
    auto asset = ai::ControllerAsset::load(directory);
    anim::Instance instance(asset->skeleton, std::make_unique<ai::Controller>(asset), asset->attributes);
    anim::Instance repeat(asset->skeleton, std::make_unique<ai::Controller>(asset), asset->attributes);
    anim::Input input;
    input.action = asset->guidances.count("Idle") ? "Idle" : asset->guidances.begin()->first;
    anim::Transform root;
    for (unsigned i = 0; i < 180; ++i) {
        if (i == 45) {
            input.desiredVelocity = {0, 0, 1};
            input.action = asset->guidances.count("Walk") ? "Walk" : "Locomotion";
        }
        if (i == 90) {
            input.facing = {1, 0, 0};
            input.desiredVelocity = {1, 0, 0};
        }
        if (i == 140) {
            input.desiredVelocity = {0, 0, 0};
            instance.reset();
            repeat.reset();
        }
        auto out = instance.evaluate(1.f / 60, root, input);
        const auto& again = repeat.evaluate(1.f / 60, root, input);
        auto model = asset->skeleton->toModel(out.localPose);
        for (size_t j = 0; j < model.size(); ++j) {
            check(std::isfinite(glm::length(model[j].position)), "Controller pose must remain finite");
            close(glm::length(model[j].rotation), 1, "Controller rotations must remain rigid", .001f);
            close(glm::distance(out.localPose[j].position, again.localPose[j].position), 0,
                  "Per-instance inference must be deterministic");
            if (asset->skeleton->joints()[j].parent >= 0)
                close(glm::length(out.localPose[j].position),
                      glm::length(asset->skeleton->joints()[j].rest.position), "Controller bone length drift",
                      .006f);
        }
        check(out.contacts.size() == 4, "Controller must publish predicted contacts");
        // Simulate a wall: accept no root translation during these frames, then resume.
        if (i < 70 || i >= 110)
            root = anim::compose(root, out.rootMotion);
    }
    std::cout << directory.filename().string() << " 180 controller ticks passed\n";
}
static void characterPoseRegression() {
    auto base = std::filesystem::path(AFTERLIGHT_ROOT) / "game/assets/animations/ai4animation";
    for (const char* kind : {"biped", "quadruped"}) {
        auto asset = ai::ControllerAsset::load(base / kind);
        const auto& skeleton = *asset->skeleton;
        auto index = [&](const char* name) {
            auto it = std::find_if(skeleton.joints().begin(), skeleton.joints().end(),
                                   [&](const anim::Joint& j) { return j.name == name; });
            check(it != skeleton.joints().end(), "Character regression bone missing");
            return size_t(it - skeleton.joints().begin());
        };
        const bool biped = asset->poseAxes;
        const auto head = index("Head");
        const auto left = index("LeftHand"), right = index("RightHand");
        auto rest = skeleton.toModel(skeleton.restPose());
        if (!biped) {
            const auto tip = index("HeadSite");
            check(skeleton.joints()[tip].parent == int(head), "Virtual muzzle joint must belong to head");
            check(rest[tip].position.z > rest[head].position.z + .15f,
                  "Dog muzzle bind direction must point forward, not back into its neck");
        }
        anim::Instance instance(asset->skeleton, std::make_unique<ai::Controller>(asset), asset->attributes);
        anim::Transform root;
        anim::Input input;
        float minimum[2] = {100, 100}, maximum[2] = {-100, -100};
        for (int frame = 0; frame < 360; ++frame) {
            if (frame == 120) {
                input.action = biped ? "Locomotion" : "Trot";
                input.desiredVelocity = {0, 0, 2};
            }
            const auto& out = instance.evaluate(1.f / 60, root, input);
            const auto model = skeleton.toModel(out.localPose);
            if (!biped && frame >= 60)
                check((model[head].rotation * vec3(0, 1, 0)).y > .8f,
                      "Head restoration must preserve upright dog ears during idle and trot");
            if (biped && frame >= 180) {
                for (int side = 0; side < 2; ++side) {
                    float z = model[side ? right : left].position.z;
                    minimum[side] = std::min(minimum[side], z);
                    maximum[side] = std::max(maximum[side], z);
                }
            }
            root = anim::compose(root, out.rootMotion);
        }
        if (biped)
            for (int side = 0; side < 2; ++side)
                check(maximum[side] - minimum[side] > .15f,
                      "Default locomotion style must animate both wrists relative to the body");
    }
}

// Offline inspection of complete native poses, including restoration and IK.
// The JSON is consumed by tools/animation/inspect_poses.py for mesh/skeleton comparisons.
static void dumpPoses(const std::filesystem::path& directory, const std::string& style) {
    std::filesystem::create_directories(directory);
    for (const char* kind : {"biped", "quadruped"}) {
        auto asset = ai::ControllerAsset::load(std::filesystem::path(AFTERLIGHT_ROOT) /
                                               "game/assets/animations/ai4animation" / kind);
        anim::Instance instance(asset->skeleton, std::make_unique<ai::Controller>(asset), asset->attributes);
        if (asset->poseAxes)
            instance.setAttribute("locomotion.style", style);
        anim::Input input;
        input.action = "Idle";
        anim::Transform root;
        std::ofstream file(directory / (std::string(kind) + ".json"));
        file << "[";
        for (int frame = 0; frame <= 360; ++frame) {
            if (frame == 120) {
                input.action = asset->poseAxes ? "Locomotion" : "Trot";
                input.desiredVelocity = {0, 0, 2};
            }
            auto model = asset->skeleton->toModel(instance.output().localPose);
            if (frame % 15 == 0) {
                if (frame)
                    file << ",";
                file << "{\"frame\":" << frame << ",\"joints\":[";
                for (size_t j = 0; j < model.size(); ++j) {
                    if (j)
                        file << ",";
                    const auto& p = model[j];
                    file << "[" << p.position.x << "," << p.position.y << "," << p.position.z << ","
                         << p.rotation.x << "," << p.rotation.y << "," << p.rotation.z << "," << p.rotation.w
                         << "]";
                }
                file << "]}";
            }
            auto out = instance.evaluate(1.f / 60, root, input);
            root = anim::compose(root, out.rootMotion);
        }
        file << "]";
    }
}
int main(int argc, char** argv) {
    try {
        if (argc >= 3 && std::string(argv[1]) == "--dump-poses") {
            dumpPoses(argv[2], argc >= 4 ? argv[3] : "Neutral");
            return 0;
        }
        framework();
        sequence();
        scriptIntegration();
        auto base = std::filesystem::path(AFTERLIGHT_ROOT) / "game/assets/animations/ai4animation";
        controller(base / "biped");
        controller(base / "quadruped");
        characterPoseRegression();
        std::cout << "Animation tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
