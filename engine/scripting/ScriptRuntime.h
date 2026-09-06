#pragma once
#include "core/World.h"
#include "assets/AssetManager.h"
#include <duktape.h>
#include <thread>
namespace afterlight {
class ScriptRuntime {
    duk_context* context_ = nullptr;
    World& world_;
    AssetManager& assets_;
    void createContext();
    void startScripts();
    void processSceneRequest();
    std::string pendingScene_;
    std::thread::id owner_;
    bool hudEnabled_ = true;
    void evaluateFile(const std::string&);
    void evaluateSource(const std::string& source, const std::string& label);
    void checkedCall(int args);

  public:
    ScriptRuntime(World&, AssetManager&);
    void loadScene(const AssetPath&);
    AssetRef saveScene(const AssetPath&, const std::string& name);
    void requestScene(std::string path) {
        pendingScene_ = std::move(path);
    }
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    void initialize();
    void setHudEnabled(bool enabled) {
        hudEnabled_ = enabled;
    }
    void execute(const std::string& source, const std::string& label = "runtime");
    void tick(float dt, const Input& input);
};
} // namespace afterlight
