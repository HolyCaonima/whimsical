#pragma once
#include "core/World.h"
#include "assets/AssetManager.h"
#include "assets/Project.h"
#include <duktape.h>
#include <thread>
#include <functional>
namespace afterlight {
namespace ui {
class UiCore;
}
class UiBindings;
class ScriptRuntime {
    duk_context* context_ = nullptr;
    World& world_;
    AssetManager& assets_;
    void createContext();
    void startScripts();
    void processSceneRequest();
    AssetRef pendingScene_;
    ContentSourceRef scriptOrigin_;
    std::vector<AssetRef> projectScripts_;
    std::thread::id owner_;
    ui::UiCore* ui_ = nullptr;
    bool hudEnabled_ = true;
    std::unique_ptr<UiBindings> uiBindings_;
    std::function<void(const std::string&)> logSink_;
    void evaluateFile(const AssetRef&);
    void evaluateSource(const std::string& source, const std::string& label);
    void checkedCall(int args);

  public:
    ScriptRuntime(World&, AssetManager&, ui::UiCore* ui = nullptr);
    void loadScene(const AssetPath&);
    void loadScene(const AssetRef&);
    AssetRef saveScene(const AssetPath&, const std::string& name);
    void requestScene(std::string path) {
        pendingScene_ = assets_.reference(AssetPath(std::move(path)));
    }
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    void initialize(); // Execute only explicitly configured scripts/current scene.
    void configure(const Project&, const std::string& mount = "/Game");
    void initialize(const Project&, const std::string& mount = "/Game");
    void setHudEnabled(bool);
    void processUiInput(Input&);
    void updateUi(float dt);
    void execute(const std::string& source, const std::string& label = "runtime");
    void setLogSink(std::function<void(const std::string&)> sink) {
        logSink_ = std::move(sink);
    }
    void log(const std::string& text);
    void tick(float dt, const Input& input);
};
} // namespace afterlight
