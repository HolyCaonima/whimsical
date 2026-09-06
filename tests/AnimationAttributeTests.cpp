#include "TestProject.h"
#include "animation/Animation.h"
#include "animation/ai4animation/Controller.h"
#include "scripting/ScriptRuntime.h"
#include "ui/EngineUi.h"
#include "render/Renderer.h"
#include <iostream>
#include <stdexcept>

using namespace afterlight;
namespace anim = afterlight::animation;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
class PeerSolver final : public anim::Solver {
  public:
    void reset(const anim::Context&) override {}
    void evaluate(const anim::Context& context, anim::Output& output) override {
        output.localPose = context.skeleton.restPose();
        output.localPose[0].position.y = context.attributes.at("test.expression") == "high" ? 1.f : 0.f;
    }
};
class PeerAsset final : public anim::Asset {
    std::shared_ptr<const anim::Skeleton> skeleton_ =
        std::make_shared<anim::Skeleton>(std::vector<anim::Joint>{{"root", -1, {}}});

  public:
    std::shared_ptr<const anim::Skeleton> skeleton() const override {
        return skeleton_;
    }
    std::unique_ptr<anim::Solver> createSolver() const override {
        return std::make_unique<PeerSolver>();
    }
    std::string solverLabel() const override {
        return "Independent peer";
    }
    std::vector<anim::EnumAttribute> attributes() const override {
        return {{"test.expression", "Expression", "low", {{"low", "Low"}, {"high", "High"}}}};
    }
};
static void framework() {
    PeerAsset asset;
    anim::Instance first(asset), second(asset);
    first.setAttribute("test.expression", "high");
    first.reset();
    first.setSolver(asset.createSolver());
    check(first.evaluate(1.f / 60, {}, {}).localPose[0].position.y == 1,
          "Peer solver must see persistent attributes after reset/replacement");
    check(second.evaluate(1.f / 60, {}, {}).localPose[0].position.y == 0,
          "Shared asset must not share mutable attribute values");
    for (auto request : {std::pair<const char*, const char*>{"test.expression", "missing"},
                         {"locomotion.style", "Zombie"}}) {
        bool rejected = false;
        try {
            first.setAttribute(request.first, request.second);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "Unknown keys/values must not silently enter the attribute set");
    }
}
static void scene() {
    World world;
    ui::UiCore uiCore(testAssets().project().content());
    ui::EngineUi engineUi(uiCore, world);
    ScriptRuntime scripts(world, testAssets(), &uiCore);
    scripts.initialize();
    auto player = world.playerId;
    auto view = world.inspectAnimation(player);
    check(view.schema.size() == 1 && view.schema[0].options.size() == 11,
          "Biped asset must declare eleven styles without Idle");
    check(view.values.at("locomotion.style") == "BigSteps", "Asset must supply the initial style");
    engineUi.sync(world.snapshot(Input{}, 0, 0, 0), RenderStatistics{});
    uiCore.snapshot();
    auto* button = engineUi.tools().GetElementById("next-0");
    check(button != nullptr, "RML must discover enum control from asset schema");
    auto position = button->GetAbsoluteOffset(Rml::BoxArea::Border);
    Input click;
    click.mouseX = position.x + 5;
    click.mouseY = position.y + 5;
    click.leftPressed = true;
    click.wheel = 2;
    auto before = world.snapshot(click, 0, 0, 0);
    scripts.processUiInput(click);
    scripts.tick(1.f / 60, click);
    check(world.inspectAnimation(player).values.at("locomotion.style") == "Chicken",
          "UI next must change style");
    check(before.animationInspection.values.at("locomotion.style") == "BigSteps",
          "Published UI values must be immutable");
    check(world.path.empty(), "Style click must not issue a ground movement command");
    check(std::abs(world.camera.distance - 30) < .001f, "Wheel over animation UI must not zoom camera");
    scripts.execute(
        "var a=Engine.animationAttributes(Locomotion.id);"
        "if(a.attributes[0].type!=='enum'||a.attributes[0].value!=='Chicken')throw Error('schema API');"
        "Engine.animationAttribute(Locomotion.id,'locomotion.style','Zombie');"
        "Engine.animationReset(Locomotion.id);");
    Input neutral;
    for (int i = 0; i < 60; ++i)
        scripts.tick(1.f / 60, neutral);
    check(world.inspectAnimation(player).values.at("locomotion.style") == "Zombie",
          "Idle input must retain style");
    scripts.execute("Locomotion.command({x:-8,y:0,z:4});");
    for (const auto& option : view.schema[0].options) {
        world.setAnimationAttribute(player, "locomotion.style", option.value);
        for (int i = 0; i < 30; ++i)
            scripts.tick(1.f / 60, neutral);
        check(world.inspectAnimation(player).values.at("locomotion.style") == option.value,
              "Gameplay movement input must not overwrite selected style");
    }
    // Prove the property changes the actual pose, not just the inspector label.
    auto resource =
        testAssets().load<anim::Asset>(AssetPath("/Game/animations/ai4animation/biped/controller"));
    anim::Instance normal(*resource), zombie(*resource);
    normal.setAttribute("locomotion.style", "Neutral");
    zombie.setAttribute("locomotion.style", "Zombie");
    anim::Input intent;
    intent.action = "Locomotion";
    intent.desiredVelocity = {0, 0, 1};
    anim::Transform a, b;
    for (int i = 0; i < 120; ++i) {
        a = anim::compose(a, normal.evaluate(1.f / 60, a, intent).rootMotion);
        b = anim::compose(b, zombie.evaluate(1.f / 60, b, intent).rootMotion);
    }
    auto n = normal.skeleton().toModel(normal.output().localPose);
    auto z = zombie.skeleton().toModel(zombie.output().localPose);
    check(z[18].position.y > n[18].position.y + .15f, "Zombie style must raise the wrist in the solved pose");
    scripts.setHudEnabled(false);
    engineUi.tools().Hide();
    auto selectedStyle = world.inspectAnimation(player).values.at("locomotion.style");
    scripts.tick(1.f / 60, click);
    check(world.inspectAnimation(player).values.at("locomotion.style") == selectedStyle,
          "Hidden HUD must not intercept scene clicks");
    world.detachAnimation(player);
    check(!world.snapshot(neutral, 1, 0, 0).animationInspection.entity,
          "Detach must remove stale UI controls");
    engineUi.tools().Show();
    uiCore.resize(1280, 700);
    uiCore.snapshot();
    check(engineUi.tools().GetElementById("next-0")->IsVisible(),
          "Resize must retain usable inspector controls");
    uiCore.resize(400, 300);
    uiCore.snapshot();
    check(!engineUi.tools().GetElementById("animation")->IsVisible(),
          "Compact windows must not leave invisible hit regions");
}
int main() {
    try {
        framework();
        scene();
        std::cout
            << "PASS: persistent attributes, peer solver, schema UI, input routing and style-driven pose\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
