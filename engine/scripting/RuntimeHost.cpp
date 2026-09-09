#include "RuntimeHost.h"
#include "scene/ScenePersistence.h"
#include "uiCore/UiCore.h"

namespace whimsical {
RuntimeHost::RuntimeHost(World& world, AssetManager& assets, ui::UiCore* ui)
    : world_(world), assets_(assets), ui_(ui) {
    simulation_ = realm();
}
RuntimeHost::~RuntimeHost() = default;
RenderView RuntimeHost::renderView() const {
    auto result = view;
    if (simulation_)
        result.outlines.insert(result.outlines.end(), simulation_->outlines.begin(), simulation_->outlines.end());
    if (application_)
        result.outlines.insert(result.outlines.end(), application_->outlines.begin(), application_->outlines.end());
    return result;
}
std::optional<std::string> RuntimeHost::openFileDialog(const OpenFileDialogOptions& options) const {
    if (!openFileDialog_)
        throw std::runtime_error("File selection is unavailable in this host");
    return openFileDialog_(options);
}
std::unique_ptr<ScriptRuntime> RuntimeHost::realm() {
    auto script = std::make_unique<ScriptRuntime>(world_, assets_, ui_, this);
    script->setLogSink(logSink_);
    script->setHudEnabled(hudEnabled_);
    return script;
}
ContentSourceRef RuntimeHost::sceneOrigin() const {
    return world_.mapAsset().id.empty() ? projectOrigin_ : assets_.origin(world_.mapAsset());
}
void RuntimeHost::configure(const Project& project, const std::string& mount) {
    projectOrigin_ = assets_.mounts()->source(mount);
    AssetManager::Scope scope(assets_, projectOrigin_, false);
    projectScripts_.clear();
    hostScripts_.clear();
    for (const auto& path : project.scripts())
        projectScripts_.push_back(assets_.reference(path));
    for (const auto& path : project.hostScripts())
        hostScripts_.push_back(assets_.reference(path));
    runOnStartup_ = project.runOnStartup();
}
void RuntimeHost::initialize(const Project& project, const std::string& mount,
                             const std::optional<AssetPath>& mapOverride) {
    configure(project, mount);
    auto path = mapOverride ? *mapOverride : project.startupMap();
    if (!path.empty()) {
        AssetRef map;
        {
            AssetManager::Scope scope(assets_, projectOrigin_, false);
            map = assets_.reference(path);
        }
        transition(map, nullptr, runOnStartup_);
    } else if (runOnStartup_)
        startProgram();
    else {
        simulation_.reset();
        running_ = false;
    }
    if (!hostScripts_.empty()) {
        application_ = realm();
        application_->start(projectOrigin_, hostScripts_);
    }
}
void RuntimeHost::initialize() {
    startProgram();
}
std::vector<AssetRef> RuntimeHost::sceneProgram(const std::optional<std::vector<AssetRef>>& libraries) const {
    std::vector<AssetRef> program;
    auto source = sceneOrigin();
    if (libraries)
        program = *libraries;
    else if (projectOrigin_ == source)
        program = projectScripts_;
    program.insert(program.end(), world_.resources.scripts.begin(), world_.resources.scripts.end());
    for (const auto& script : program) {
        if (assets_.origin(script) != source)
            throw std::invalid_argument("A scene program must belong to the scene Content");
        (void)assets_.load<ScriptAsset>(script);
    }
    return program;
}
void RuntimeHost::startProgram(const std::optional<std::vector<AssetRef>>& libraries) {
    auto program = sceneProgram(libraries);
    simulation_ = realm();
    running_ = false;
    paused_ = false;
    simulation_->start(sceneOrigin(), program);
    running_ = true;
}
void RuntimeHost::transition(const AssetRef& source, const SceneDocument* document, bool run) {
    auto observers = world_.deferObservers();
    std::exception_ptr publicationFailure;
    try {
        if (document)
            ScenePersistence::restore(world_, assets_, *document, source);
        else {
            auto ref = assets_.resolve(source);
            ScenePersistence::load(world_, assets_, ref.path);
        }
    } catch (const ScenePersistence::CommittedError&) {
        publicationFailure = std::current_exception();
    }
    // Validation failed above leaves both old scene and realms intact. Once committed,
    // retire the scene realm before publishing changes to application observers.
    simulation_.reset();
    running_ = paused_ = false;
    simulationTime_ = 0;
    checkpoint_.reset();
    if (run)
        startProgram();
    changed();
    observers.commit();
    if (publicationFailure)
        std::rethrow_exception(publicationFailure);
}
void RuntimeHost::changed() {
    if (application_)
        application_->notify("sceneChanged");
}
void RuntimeHost::loadScene(const AssetPath& path) {
    loadScene(assets_.reference(path));
}
void RuntimeHost::loadScene(const AssetRef& ref) {
    transition(ref, nullptr, true);
}
void RuntimeHost::openScene(const AssetRef& ref) {
    transition(ref, nullptr, false);
}
void RuntimeHost::restoreScene(const SceneDocument& document, const AssetRef& source) {
    transition(source, &document, false);
}
SceneDocument RuntimeHost::captureScene() const {
    auto origin = sceneOrigin();
    AssetManager::Scope scope(assets_, origin, bool(origin));
    return ScenePersistence::capture(world_, assets_);
}
AssetRef RuntimeHost::saveScene(const AssetPath& path, const std::string& name) {
    return ScenePersistence::save(world_, assets_, path, name);
}
void RuntimeHost::play(const std::optional<std::vector<AssetRef>>& libraries) {
    if (running_)
        throw std::logic_error("Simulation is already running");
    (void)sceneProgram(libraries);
    checkpoint_ = Checkpoint{captureScene(), world_.mapAsset()};
    simulationTime_ = 0;
    startProgram(libraries);
}
void RuntimeHost::stop() {
    if (checkpoint_) {
        // Copy before transition resets the checkpoint. Restore uses the same staged
        // loader as disk maps; stale entities and RT requests keep their invalidation rules.
        auto checkpoint = *checkpoint_;
        transition(checkpoint.source, &checkpoint.document, false);
    } else {
        simulation_.reset();
        running_ = paused_ = false;
    }
}
void RuntimeHost::requestScene(const AssetRef& ref, bool run) {
    pending_.push_back({run ? Action::RunMap : Action::Load, assets_.resolve(ref)});
}
void RuntimeHost::requestRestore(SceneDocument document, AssetRef ref) {
    pending_.push_back({Action::Restore, std::move(ref), std::move(document)});
}
void RuntimeHost::requestPlay(std::optional<std::vector<AssetRef>> scripts) {
    pending_.push_back({Action::Play, {}, {}, std::move(scripts)});
}
void RuntimeHost::requestStop() {
    pending_.push_back({Action::Stop});
}
void RuntimeHost::requestPause(bool value) {
    pending_.push_back({Action::Pause, {}, {}, {}, value});
}
void RuntimeHost::processRequests() {
    // Requests produced while starting the next program belong to the next boundary.
    auto requests = std::move(pending_);
    pending_.clear();
    try {
        for (auto& request : requests) {
            switch (request.action) {
            case Action::Load:
                transition(request.source, nullptr, false);
                break;
            case Action::RunMap:
                transition(request.source, nullptr, true);
                break;
            case Action::Restore:
                transition(request.source, &*request.document, false);
                break;
            case Action::Play:
                play(request.scripts);
                break;
            case Action::Stop:
                stop();
                break;
            case Action::Pause:
                pause(request.paused);
                break;
            }
        }
    } catch (...) {
        pending_.clear();
        throw;
    }
}
void RuntimeHost::tick(float realDt, const Input& input, float simulationDt) {
    if (application_) {
        application_->tick(realDt, input, false);
        processRequests();
    }
    if (running_ && !paused_ && simulationDt > 0) {
        if (simulation_) {
            auto sceneInput = input;
            auto rect = view.rectangle.fit(input.width, input.height);
            sceneInput.pointerCaptured = input.pointerCaptured || !rect.contains(input.mouseX, input.mouseY);
            sceneInput.mouseX -= rect.x;
            sceneInput.mouseY -= rect.y;
            sceneInput.width = rect.width;
            sceneInput.height = rect.height;
            simulation_->tick(simulationDt, sceneInput, true);
        }
        world_.update(simulationDt);
        simulationTime_ += simulationDt;
        processRequests();
    }
    updateUi(realDt);
}
void RuntimeHost::updateUi(float dt) {
    if (simulation_)
        simulation_->updateUi(dt);
    if (application_)
        application_->updateUi(dt);
    processRequests();
}
void RuntimeHost::processUiInput(Input& input) {
    if (ui_)
        ui_->processInput(input);
    processRequests();
}
void RuntimeHost::setHudEnabled(bool enabled) {
    hudEnabled_ = enabled;
    if (simulation_)
        simulation_->setHudEnabled(enabled);
    if (application_)
        application_->setHudEnabled(enabled);
}
void RuntimeHost::setLogSink(std::function<void(const std::string&)> sink) {
    logSink_ = std::move(sink);
    if (simulation_)
        simulation_->setLogSink(logSink_);
    if (application_)
        application_->setLogSink(logSink_);
}
void RuntimeHost::execute(const std::string& source, const std::string& label) {
    if (!simulation_) {
        simulation_ = realm();
        simulation_->start(sceneOrigin(), {});
    }
    simulation_->execute(source, label);
    processRequests();
}
void RuntimeHost::executeHost(const std::string& source, const std::string& label) {
    if (!application_) {
        application_ = realm();
        application_->start(projectOrigin_, {});
    }
    application_->execute(source, label);
    processRequests();
}
} // namespace whimsical
