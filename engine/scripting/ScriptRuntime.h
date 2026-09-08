#pragma once
#include "core/World.h"
#include "assets/AssetManager.h"
#include <duktape.h>
#include <thread>
#include <functional>
#include <set>
namespace afterlight {
namespace ui {
class UiCore;
}
class UiBindings;
class RuntimeHost;
// One realm has a fixed Content origin. It executes callbacks but never advances a
// World or owns scene transitions; RuntimeHost composes those independent operations.
class ScriptRuntime {
    duk_context* context_ = nullptr;
    World& world_;
    AssetManager& assets_;
    void createContext();
    ContentSourceRef scriptOrigin_;
    RuntimeHost* host_;
    std::thread::id owner_;
    ui::UiCore* ui_ = nullptr;
    bool hudEnabled_ = true;
    std::unique_ptr<UiBindings> uiBindings_;
    std::function<void(const std::string&)> logSink_;
    std::set<std::string> pixelReads_;
    bool started_ = false;
    void evaluateFile(const AssetRef&);
    void evaluateSource(const std::string& source, const std::string& label);
    void checkedCall(int args);

  public:
    ScriptRuntime(World&, AssetManager&, ui::UiCore* ui = nullptr, RuntimeHost* host = nullptr);
    RuntimeHost& host() const;
    void trackPixelRead(const std::string& id) {
        pixelReads_.insert(id);
    }
    void forgetPixelRead(const std::string& id) {
        pixelReads_.erase(id);
    }
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    void start(ContentSourceRef, const std::vector<AssetRef>&);
    void notify(const char* callback);
    void setHudEnabled(bool);
    void updateUi(float dt);
    void execute(const std::string& source, const std::string& label = "runtime");
    void setLogSink(std::function<void(const std::string&)> sink) {
        logSink_ = std::move(sink);
    }
    void log(const std::string& text);
    void tick(float dt, const Input& input, bool gameplayInput = true);
};
} // namespace afterlight
