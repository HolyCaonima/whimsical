#pragma once
#include "ScriptRuntime.h"
#include "assets/Project.h"
#include "scene/SceneAsset.h"
#include "platform/FileDialog.h"
#include <deque>

namespace whimsical {
// Application composition for one active scene. Realms execute code; the host owns
// their lifetimes, scene transitions, simulation scheduling and the presented view.
class RuntimeHost {
    World& world_;
    AssetManager& assets_;
    ui::UiCore* ui_;
    std::unique_ptr<ScriptRuntime> application_, simulation_;
    ContentSourceRef projectOrigin_;
    std::vector<AssetRef> projectScripts_, hostScripts_;
    bool runOnStartup_ = true, running_ = true, paused_ = false, hudEnabled_ = true;
    double simulationTime_ = 0;
    std::function<void(const std::string&)> logSink_;
    std::function<std::optional<std::string>(const OpenFileDialogOptions&)> openFileDialog_;
    struct Checkpoint {
        SceneDocument document;
        AssetRef source;
    };
    std::optional<Checkpoint> checkpoint_;
    enum class Action { Load, RunMap, Restore, Play, Stop, Pause };
    struct Request {
        Action action;
        AssetRef source;
        std::optional<SceneDocument> document;
        std::optional<std::vector<AssetRef>> scripts;
        bool paused = false;
    };
    std::deque<Request> pending_;
    std::unique_ptr<ScriptRuntime> realm();
    ContentSourceRef sceneOrigin() const;
    std::vector<AssetRef> sceneProgram(const std::optional<std::vector<AssetRef>>&) const;
    void startProgram(const std::optional<std::vector<AssetRef>>& = {});
    void transition(const AssetRef&, const SceneDocument*, bool run);
    void changed();

  public:
    RenderView view;
    RenderView renderView() const;
    RuntimeHost(World&, AssetManager&, ui::UiCore* = nullptr);
    ~RuntimeHost();
    RuntimeHost(const RuntimeHost&) = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;
    void configure(const Project&, const std::string& mount = "/Game");
    void initialize(const Project&, const std::string& mount = "/Game",
                    const std::optional<AssetPath>& mapOverride = {});
    void initialize();
    // Game navigation composes data load + scene program restart. Host scripts survive.
    void loadScene(const AssetPath&);
    void loadScene(const AssetRef&);
    // Data-only replacement never runs target scripts and does not replace the host realm.
    void openScene(const AssetRef&);
    void restoreScene(const SceneDocument&, const AssetRef&);
    SceneDocument captureScene() const;
    AssetRef saveScene(const AssetPath&, const std::string&);
    void play(const std::optional<std::vector<AssetRef>>& libraries = {});
    void stop();
    void pause(bool value) {
        paused_ = value;
    }
    bool running() const {
        return running_;
    }
    bool paused() const {
        return paused_;
    }
    double simulationTime() const {
        return simulationTime_;
    }
    void requestScene(const AssetRef&, bool run);
    void requestRestore(SceneDocument, AssetRef);
    void requestPlay(std::optional<std::vector<AssetRef>>);
    void requestStop();
    void requestPause(bool);
    void processRequests(); // Only at a protected-call boundary, never inside a JS callback.
    void tick(float dt, const Input& input) {
        tick(dt, input, dt);
    }
    void tick(float realDt, const Input&, float simulationDt);
    void updateUi(float);
    void processUiInput(Input&);
    void setHudEnabled(bool);
    void setLogSink(std::function<void(const std::string&)>);
    void setOpenFileDialog(std::function<std::optional<std::string>(const OpenFileDialogOptions&)> picker) {
        openFileDialog_ = std::move(picker);
    }
    std::optional<std::string> openFileDialog(const OpenFileDialogOptions&) const;
    void execute(const std::string&, const std::string& label = "runtime");
    void executeHost(const std::string&, const std::string& label = "host");
};
} // namespace whimsical
