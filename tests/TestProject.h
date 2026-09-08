#pragma once
#include "assets/EngineAssets.h"
#include "assets/Project.h"
#include "animation/ai4animation/Controller.h"
inline afterlight::Project testProject() {
    return afterlight::Project(std::filesystem::path(AFTERLIGHT_ROOT) / "Projects" / "Afterlight");
}
inline afterlight::AssetManager& testAssets() {
    static auto assets = [] {
        auto a = std::make_unique<afterlight::AssetManager>(testProject().content());
        a->mount("/Engine", std::filesystem::path(AFTERLIGHT_ROOT) / "engine/Content", false);
        afterlight::registerEngineAssets(*a);
        return a;
    }();
    return *assets;
}
inline std::shared_ptr<const afterlight::animation::ai4animation::ControllerAsset>
testController(const std::string& kind) {
    return testAssets()
        .load<afterlight::animation::ai4animation::ControllerResource>(
            afterlight::AssetPath("/Game/animations/ai4animation/" + kind + "/controller"))
        ->data;
}
