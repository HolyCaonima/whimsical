#include "core/World.h"
#include "assets/EngineAssets.h"
#include "core/FrameMailbox.h"
#include "scripting/ScriptRuntime.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "ui/EngineUi.h"
#include "ui/Console.h"
#include "core/EngineSettings.h"
#include "debug/ConsoleSmoke.h"
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <mutex>
using namespace afterlight;
int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    try {
        RenderOptions options;
        uint32_t width = 1280, height = 800;
        int debugView = 0;
        bool demo = false, smoke = false;
        bool consoleOpen = false, consoleSmoke = false;
        std::vector<std::string> startupCommands;
        uint32_t stress = 0;
        std::filesystem::path projectPath =
            std::filesystem::path(AFTERLIGHT_ROOT) / "Projects" / "Afterlight";
        std::string mapPath;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            auto number = [&]() {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing value for " + arg);
                return std::stoi(argv[++i]);
            };
            if (arg == "--project" || arg == "--map") {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing value for " + arg);
                if (arg == "--project")
                    projectPath = argv[++i];
                else
                    mapPath = argv[++i];
            } else if (arg == "--frames")
                options.maxFrames = uint32_t(std::max(1, number()));
            else if (arg == "--width")
                width = uint32_t(std::clamp(number(), 640, 2560));
            else if (arg == "--height")
                height = uint32_t(std::clamp(number(), 400, 1440));
            else if (arg == "--view") {
                debugView = std::clamp(number(), 0, 7);
                startupCommands.push_back("r.DebugView " + std::to_string(debugView));
            } else if (arg == "--capture")
                options.capture = true;
            else if (arg == "--no-hud") {
                options.hud = false;
                startupCommands.push_back("r.Hud false");
            } else if (arg == "--physics-debug") {
                startupCommands.push_back("p.DebugDraw true");
            } else if (arg == "--audit") {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing audit name");
                options.audit = argv[++i];
                if (options.audit.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                    std::string::npos)
                    throw std::runtime_error("Audit name must be a simple directory name");
            } else if (arg == "--audit-motion")
                options.auditMotion = true;
            else if (arg == "--audit-occluder")
                options.auditOccluder = number();
            else if (arg == "--no-validation") {
                options.validation = false;
                startupCommands.push_back("r.Validation false");
            } else if (arg == "--validation") {
                options.validation = true;
                startupCommands.push_back("r.Validation true");
            } else if (arg == "--present") {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing value for --present");
                std::string mode = argv[++i];
                if (mode == "fifo")
                    options.present = PresentMode::Fifo;
                else if (mode == "mailbox")
                    options.present = PresentMode::Mailbox;
                else if (mode == "immediate")
                    options.present = PresentMode::Immediate;
                else
                    throw std::runtime_error("--present expects fifo, mailbox or immediate");
                startupCommands.push_back("r.Present " + mode);
            } else if (arg == "--demo")
                demo = true;
            else if (arg == "--smoke")
                smoke = true;
            else if (arg == "--stress")
                stress = uint32_t(std::clamp(number(), 0, 900));
            else if (arg == "--full-upload") {
                options.fullUpload = true;
                startupCommands.push_back("r.FullUpload true");
            } else if (arg == "--console")
                consoleOpen = true;
            else if (arg == "--console-smoke")
                consoleSmoke = true;
            else if (arg == "--profile-gpu")
                startupCommands.push_back("profileGPU");
            else if (arg == "--profile-cpu")
                startupCommands.push_back("profileCPU");
            else if (arg == "--exec" || arg == "--cvar") {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing value for " + arg);
                startupCommands.push_back(argv[++i]);
            } else if (arg == "--help") {
                std::cout << "Afterlight [--frames N] [--capture] [--width 1280] [--height 800] [--view "
                             "0..7] [--no-hud] [--validation] [--no-validation] [--present "
                             "fifo|mailbox|immediate] [--demo] [--smoke] [--stress N] [--full-upload] "
                             "[--audit NAME] [--audit-motion] [--audit-occluder SLOT] [--physics-debug] [--project DIR] [--map "
                             "/Game/Maps/Name] [--console] [--exec \"command\"] [--cvar \"name=value\"] "
                             "[--console-smoke] [--profile-gpu] [--profile-cpu]\n";
                return 0;
            } else
                throw std::runtime_error("Unknown option " + arg);
        }
        if (options.auditMotion && options.audit.empty())
            throw std::runtime_error("--audit-motion requires --audit NAME");
        if (options.auditOccluder >= 0 && options.audit.empty())
            throw std::runtime_error("--audit-occluder requires --audit NAME");
        if (!options.audit.empty()) {
            if (!options.maxFrames)
                options.maxFrames = 192;
            if (options.maxFrames <= 64)
                throw std::runtime_error("--audit requires at least 65 frames (64 warmup frames)");
            if (smoke || demo)
                throw std::runtime_error("--audit uses a frozen scene; use --smoke/--demo in a separate run");
            options.hud = false;
        }
        if (options.capture && !options.maxFrames)
            options.maxFrames = 90;
        if (smoke || consoleSmoke) {
            options.maxFrames = 160;
            options.capture = true;
            // The smoke scenario is the project's correctness gate, so it always pays for
            // validation regardless of the build configuration default.
            options.validation = true;
        }
        if (consoleSmoke && (smoke || demo || !options.audit.empty()))
            throw std::runtime_error("--console-smoke requires its own run");
        std::cout << "AFTERLIGHT | C++ engine / JavaScript gameplay / Vulkan RT\n";
        Window window(width, height);
        AssetManager assets{Project(projectPath)};
        registerEngineAssets(assets);
        ConsoleRegistry variables;
        EngineSettings settings(variables, RenderOptions{}.validation);
        ui::Console console(variables);
        console.open(consoleOpen);
        const auto savedConfig = assets.project().root() / "Saved" / "ConsoleVariables.cfg";
        auto loadConfig = [&](const std::filesystem::path& path) {
            if (!std::filesystem::exists(path))
                return;
            std::ifstream file(path);
            if (!file)
                throw std::runtime_error("Cannot open console config: " + path.string());
            console.log("Config: " + path.string());
            for (const auto& result : variables.load(file))
                console.log(result.text, !result.ok);
        };
        auto loadConfigs = [&] {
            loadConfig(assets.project().root() / "Config" / "ConsoleVariables.cfg");
            loadConfig(savedConfig);
        };
        variables.command("cvar.save", "Save archive CVars to this project's Saved/ConsoleVariables.cfg",
                          [&](const auto& args) {
                              if (!args.empty())
                                  throw std::runtime_error("Usage: cvar.save");
                              std::filesystem::create_directories(savedConfig.parent_path());
                              std::ofstream file(savedConfig);
                              variables.save(file);
                              return "Saved: " + savedConfig.string();
                          });
        variables.command("cvar.load", "Reload project and saved configs; higher-priority overrides remain",
                          [&](const auto& args) {
                              if (!args.empty())
                                  throw std::runtime_error("Usage: cvar.load");
                              loadConfigs();
                              return "Configuration loaded; see assignment results above.";
                          });
        loadConfigs();
        World world;
        ui::UiCore uiCore(assets.project().content());
        ui::EngineUi engineUi(uiCore);
        ScriptRuntime scripts(world, assets, &uiCore);
        scripts.setLogSink([&](const auto& text) { console.log(text); });
        if (mapPath.empty())
            scripts.initialize();
        else
            scripts.loadScene(AssetPath(mapPath));
        // A level-sized scene in which the number of objects that change each tick stays
        // fixed no matter how many exist. That is the measurement that separates the two
        // architectures: per-frame work proportional to the scene climbs with --stress,
        // per-frame work proportional to the change does not.
        std::vector<uint32_t> stressMoving;
        if (stress) {
            const auto side = uint32_t(std::ceil(std::sqrt(double(stress))));
            for (uint32_t i = 0; i < stress; i++) {
                const float x = -11.f + float(i % side) * 22.f / float(side);
                const float z = -8.f + float(i / side) * 16.f / float(side);
                const auto id =
                    world.spawn("Stress prop", Shape::Box, {x, .3f, z}, {.25f, .6f, .25f}, 0, false, false);
                if (stressMoving.size() < 8)
                    stressMoving.push_back(id);
            }
            std::cout << "Stress scene: " << stress << " props, " << stressMoving.size()
                      << " of them moving every tick\n";
        }
        if (!options.audit.empty()) {
            Input neutral;
            neutral.width = width;
            neutral.height = height;
            for (int i = 0; i < 600; ++i)
                scripts.tick(1.f / 60.f, neutral);
        }
        FrameMailbox mailbox;
        std::atomic<bool> finished{false};
        std::atomic<uint64_t> rendered{0};
        std::atomic<double> renderFps{-1}, renderFrameMs{0}, renderCpuMs{0}, renderGpuMs{0};
        std::atomic<bool> renderVsync{true};
        uint64_t gpuProfileRequest = 0, gpuProfileCompleted = 0;
        std::mutex gpuProfileMutex;
        std::optional<GpuProfile> gpuProfileResult, lastGpuProfile;
        uint64_t cpuProfileRequest = 0, cpuProfilePrepared = 0, cpuProfileCompleted = 0;
        std::mutex cpuProfileMutex;
        std::optional<CpuProfile> cpuProfileResult, lastCpuProfile;
        std::unique_ptr<CpuProfiler> gameCpuProfiler;
        std::shared_ptr<const CpuProfile> cpuSnapshot;
        variables.command(
            "profileCPU",
            "profileCPU [last]: capture the next Game/Render CPU frame, or show the last report",
            [&](const auto& args) {
                if (args.size() == 1 && args[0] == "last")
                    return lastCpuProfile ? lastCpuProfile->text()
                                          : std::string("No CPU profile captured yet.");
                if (!args.empty())
                    throw std::runtime_error("Usage: profileCPU [last]");
                if (cpuProfileRequest != cpuProfileCompleted)
                    return std::string("CPU profile already pending; waiting for the next rendered frame.");
                ++cpuProfileRequest;
                return "CPU profile request " + std::to_string(cpuProfileRequest) +
                       " queued for the next Game/Render frame.";
            });
        auto prepareCpuProfile = [&] {
            if (cpuProfileRequest > cpuProfilePrepared)
                gameCpuProfiler = std::make_unique<CpuProfiler>(true, "Game", "Game Update");
        };
        auto collectCpuProfile = [&] {
            std::optional<CpuProfile> result;
            {
                std::lock_guard<std::mutex> lock(cpuProfileMutex);
                result = std::move(cpuProfileResult);
                cpuProfileResult.reset();
            }
            if (!result)
                return false;
            cpuProfileCompleted = result->request;
            cpuSnapshot.reset();
            lastCpuProfile = std::move(result);
            const auto report = lastCpuProfile->text();
            console.log(report);
            std::cout << report;
            try {
                const auto directory = std::filesystem::path(AFTERLIGHT_ROOT) / "captures";
                std::filesystem::create_directories(directory);
                std::ofstream json(directory / "cpu-profile.json"), text(directory / "cpu-profile.txt");
                json << lastCpuProfile->json();
                text << report;
                if (!json || !text)
                    throw std::runtime_error("Could not save CPU profile files");
                const auto saved = "CPU profile saved: " + (directory / "cpu-profile.json").generic_string();
                console.log(saved);
                std::cout << saved << '\n';
            } catch (const std::exception& error) {
                console.log(error.what(), true);
                std::cerr << "[ProfileCPU] " << error.what() << '\n';
            }
            return true;
        };
        variables.command(
            "profileGPU", "profileGPU [last]: capture the next GPU frame, or show the last report",
            [&](const auto& args) {
                if (args.size() == 1 && args[0] == "last")
                    return lastGpuProfile ? lastGpuProfile->text()
                                          : std::string("No GPU profile captured yet.");
                if (!args.empty())
                    throw std::runtime_error("Usage: profileGPU [last]");
                if (gpuProfileRequest != gpuProfileCompleted)
                    return std::string("GPU profile already pending; waiting for the next rendered frame.");
                ++gpuProfileRequest;
                return "GPU profile request " + std::to_string(gpuProfileRequest) +
                       " queued for the next rendered frame.";
            });
        auto collectGpuProfile = [&] {
            std::optional<GpuProfile> result;
            {
                std::lock_guard<std::mutex> lock(gpuProfileMutex);
                result = std::move(gpuProfileResult);
                gpuProfileResult.reset();
            }
            if (!result)
                return false;
            gpuProfileCompleted = result->request;
            lastGpuProfile = std::move(result);
            const auto report = lastGpuProfile->text();
            console.log(report, !lastGpuProfile->error.empty());
            std::cout << report;
            try {
                const auto directory = std::filesystem::path(AFTERLIGHT_ROOT) / "captures";
                std::filesystem::create_directories(directory);
                std::ofstream json(directory / "gpu-profile.json"), text(directory / "gpu-profile.txt");
                json << lastGpuProfile->json();
                text << report;
                if (!json || !text)
                    throw std::runtime_error("Could not save GPU profile files");
                const auto saved = "GPU profile saved: " + (directory / "gpu-profile.json").generic_string();
                console.log(saved);
                std::cout << saved << '\n';
            } catch (const std::exception& error) {
                console.log(error.what(), true);
                std::cerr << "[ProfileGPU] " << error.what() << '\n';
            }
            return true;
        };
        bool quitRequested = false;
        variables.command("quit", "Close the running engine", [&](const auto& args) {
            if (!args.empty())
                throw std::runtime_error("Usage: quit");
            quitRequested = true;
            return "Shutting down.";
        });
        variables.command("map.reload", "Reload the current Map; retain console variables",
                          [&](const auto& args) {
                              if (!args.empty())
                                  throw std::runtime_error("Usage: map.reload");
                              scripts.loadScene(world.mapAsset().path);
                              stressMoving.clear();
                              return "Map reloaded.";
                          });
        variables.command("stat", "Show current frame/CPU (Render)/GPU timing and scene object count",
                          [&](const auto& args) {
                              if (!args.empty())
                                  throw std::runtime_error("Usage: stat");
                              std::ostringstream out;
                              if (renderFps.load() < 0)
                                  out << "Frame timing warming up";
                              else
                                  out << "FPS " << renderFps.load() << " | Frame " << renderFrameMs.load()
                                      << " ms | CPU (Render) " << renderCpuMs.load() << " ms | GPU "
                                      << renderGpuMs.load() << " ms";
                              out << " | Scene objects "
                                  << std::count_if(world.objects().begin(), world.objects().end(),
                                                   [](const auto& object) { return object.alive; });
                              return out.str();
                          });
        for (const auto& command : startupCommands) {
            auto result = console.execute(command, CVarSource::CommandLine);
            if (!result.ok)
                throw std::runtime_error("Startup console command: " + result.text);
        }
        if (smoke || consoleSmoke)
            variables.set("r.Validation", "true", CVarSource::CommandLine);
        if (!options.audit.empty())
            variables.set("r.Hud", "false", CVarSource::CommandLine);
        options.hud = settings.hud();
        scripts.setHudEnabled(options.hud);
        options.validation = variables.get<bool>("r.Validation");
        auto present = variables.get<std::string>("r.Present");
        options.present = present == "fifo"      ? PresentMode::Fifo
                          : present == "mailbox" ? PresentMode::Mailbox
                                                 : PresentMode::Immediate;
        options.fullUpload = false; // The live Frame value is the single source of truth.
        variables.finishStartup();
        std::string renderError;
        uint32_t validationErrors = 0;
        std::thread renderThread([&] {
            try {
                Renderer renderer(window.handle(), options);
                FrameRef frame;
                uint64_t seen = 0;
                for (;;) {
                    // Only blocks before the first snapshot: afterwards the newest one is
                    // re-presented rather than stalling for the next simulation tick.
                    if (mailbox.acquire(frame, seen) == FrameStatus::Closed)
                        break;
                    bool more = renderer.render(frame);
                    if (auto result = renderer.takeCpuProfile()) {
                        std::lock_guard<std::mutex> lock(cpuProfileMutex);
                        cpuProfileResult = std::move(result);
                    }
                    if (auto result = renderer.takeGpuProfile()) {
                        std::lock_guard<std::mutex> lock(gpuProfileMutex);
                        gpuProfileResult = std::move(result);
                    }
                    rendered.store(renderer.frames());
                    const auto stats = renderer.statistics();
                    renderFps.store(stats.fps);
                    renderFrameMs.store(stats.frameMs);
                    renderCpuMs.store(stats.cpuMs);
                    renderGpuMs.store(stats.gpuMs);
                    renderVsync.store(stats.vsync);
                    if (!more)
                        break;
                }
                validationErrors = renderer.errors();
            } catch (const std::exception& error) {
                renderError = error.what();
            }
            finished.store(true);
            mailbox.close();
            window.wake(); // Release the simulation thread from its pacing wait at once.
        });
        using Clock = std::chrono::steady_clock;
        auto previous = Clock::now(), lastTitle = previous;
        double accumulator = 0, time = 0;
        uint64_t tick = 0;
        constexpr double step = 1.0 / 60.0;
        std::string mainError;
        uint32_t smokeStage = 0;
        ConsoleSmoke consoleCheck;
        auto restoreAt = Clock::time_point::max();
        auto publish = [&] {
            Frame frame;
            {
                CpuScope scope("Build Snapshot / Console View");
                frame =
                    world.snapshot(window.input(), tick, time, settings.debugView(), settings.physicsDebug());
                settings.decorate(frame);
                frame.gpuProfileRequest = gpuProfileRequest;
                frame.console = console.view();
                RenderStatistics statistics;
                statistics.fps = renderFps.load();
                statistics.frameMs = renderFrameMs.load();
                statistics.cpuMs = renderCpuMs.load();
                statistics.gpuMs = renderGpuMs.load();
                statistics.vsync = renderVsync.load();
                statistics.present = present.c_str();
                engineUi.sync(frame, statistics);
                frame.ui = engineUi.snapshot(frame);
            }
            if (gameCpuProfiler) {
                CpuProfile report;
                report.request = cpuProfileRequest;
                report.tick = tick;
                report.threads.push_back(gameCpuProfiler->finish());
                gameCpuProfiler.reset();
                cpuSnapshot = std::make_shared<const CpuProfile>(std::move(report));
                cpuProfilePrepared = cpuProfileRequest;
            }
            frame.cpuProfile = cpuSnapshot;
            mailbox.publish(std::move(frame));
        };
        prepareCpuProfile();
        publish();
        try {
            while (window.pump() && !finished.load() && !quitRequested) {
                auto now = Clock::now();
                accumulator += std::min(std::chrono::duration<double>(now - previous).count(), .1);
                previous = now;
                bool changed = collectGpuProfile();
                changed = collectCpuProfile() || changed;
                if (accumulator >= step)
                    prepareCpuProfile();
                while (accumulator >= step) {
                    CpuScope tickScope("Fixed Tick");
                    Input input = window.input();
                    if (consoleSmoke)
                        consoleCheck.update(rendered.load(), window, console, variables);
                    if (smoke || demo)
                        input.uiEvents.clear(); // These scenarios inject aggregate input below.
                    if (demo) {
                        auto f = rendered.load();
                        input.keys['W'] = f >= 15 && f < 65;
                        input.keys[VK_SHIFT] = f >= 35 && f < 65;
                    }
                    if (smoke) {
                        auto f = rendered.load();
                        if (smokeStage == 0 && f >= 12) {
                            auto clip = world.camera.projection(float(input.width) / input.height) *
                                        world.camera.view() * vec4(-8, 0, 4, 1);
                            input.mouseX = (clip.x / clip.w * .5f + .5f) * input.width;
                            input.mouseY = (clip.y / clip.w * .5f + .5f) * input.height;
                            input.leftPressed = true;
                            smokeStage++;
                            std::cout << "[Smoke] Click command\n";
                        } else if (smokeStage == 1 && f >= 40) {
                            input.middle = true;
                            input.deltaX = 42;
                            input.deltaY = 15;
                            input.wheel = 2;
                            smokeStage++;
                            std::cout << "[Smoke] Camera orbit / zoom\n";
                        } else if (smokeStage == 2 && f >= 55) {
                            window.resizeClient(1100, 700);
                            smokeStage++;
                            std::cout << "[Smoke] Resize\n";
                        } else if (smokeStage == 3 && f >= 80) {
                            window.minimize(true);
                            restoreAt = now + std::chrono::milliseconds(250);
                            smokeStage++;
                            std::cout << "[Smoke] Minimize\n";
                        } else if (smokeStage == 4 && now >= restoreAt) {
                            window.minimize(false);
                            smokeStage++;
                            std::cout << "[Smoke] Restore\n";
                        } else if (smokeStage == 5 && f >= 110) {
                            input.pressed[VK_ESCAPE] = true;
                            smokeStage++;
                            std::cout << "[Smoke] Cancel movement\n";
                        } else if (smokeStage == 6 && f >= 125) {
                            scripts.loadScene(world.mapAsset().path);
                            stressMoving.clear(); // These transient entities belonged to the previous scene.
                            smokeStage++;
                            std::cout << "[Smoke] Reload Map / rebuild gameplay bindings\n";
                        }
                    }
                    const bool wasOpen = console.isOpen();
                    {
                        CpuScope scope("Console Input / Commands");
                        console.handle(input);
                    }
                    if (wasOpen != console.isOpen())
                        window.releaseGameInput();
                    if (input.pressed[VK_F2])
                        variables.set("p.DebugDraw", settings.physicsDebug() ? "false" : "true",
                                      CVarSource::Console);
                    {
                        CpuScope scope("UI / Input");
                        scripts.setHudEnabled(settings.hud());
                        scripts.processUiInput(input);
                    }
                    const double gameStep = step * settings.timeScale();
                    if (options.audit.empty() && gameStep > 0) {
                        CpuScope scope("Script / Gameplay Tick");
                        scripts.tick(float(gameStep), input);
                    } else
                        scripts.updateUi(float(step));
                    for (size_t i = 0; i < stressMoving.size(); i++) {
                        const auto& prop = world.entity(stressMoving[i]);
                        world.setPose(stressMoving[i],
                                      {prop.position.x, .3f + .2f * float(std::sin(time * 2 + double(i))),
                                       prop.position.z},
                                      0, .6f);
                    }
                    window.consumeEdges();
                    accumulator -= step;
                    if (options.audit.empty())
                        time += gameStep;
                    tick++;
                    changed = true;
                }
                if (changed || gameCpuProfiler)
                    publish();
                if (now - lastTitle > std::chrono::milliseconds(500)) {
                    std::ostringstream title;
                    title << "AFTERLIGHT | The Rain Court | " << world.state << " | ";
                    const double fps = renderFps.load();
                    if (!window.input().width || !window.input().height)
                        title << "Paused (minimized)";
                    else if (fps < 0)
                        title << "Measuring FPS...";
                    else
                        title << std::fixed << std::setprecision(1) << fps << " FPS | "
                              << std::setprecision(2) << "Frame " << renderFrameMs.load()
                              << " ms | CPU (Render) " << renderCpuMs.load() << " ms | GPU "
                              << renderGpuMs.load() << " ms | "
                              << (renderVsync.load() ? "VSync ON" : "VSync OFF");
                    window.title(title.str());
                    lastTitle = now;
                }
                // Sleep exactly until the next fixed step is due instead of polling, and
                // wake early for input or for the render thread finishing. Deadlines are
                // measured from after this iteration's work, not from the top of it.
                const auto worked = Clock::now();
                const double spent = std::chrono::duration<double>(worked - now).count();
                const double untilStep = step - accumulator - spent;
                const double untilTitle = .5 - std::chrono::duration<double>(worked - lastTitle).count();
                window.waitForMessages(uint32_t(std::max(0., std::min(untilStep, untilTitle)) * 1000));
            }
        } catch (const std::exception& e) {
            mainError = e.what();
        }
        mailbox.close();
        renderThread.join();
        gameCpuProfiler.reset();
        collectGpuProfile();
        collectCpuProfile();
        if (cpuProfileRequest != cpuProfileCompleted)
            std::cout << "[ProfileCPU] Pending request was not captured before shutdown.\n";
        if (gpuProfileRequest != gpuProfileCompleted)
            std::cout << "[ProfileGPU] Pending request was not captured before shutdown.\n";
        if (!mainError.empty())
            throw std::runtime_error(mainError);
        if (!renderError.empty())
            throw std::runtime_error(renderError);
        if (smoke && smokeStage != 7)
            throw std::runtime_error("Smoke scenario did not complete");
        if (consoleSmoke && !consoleCheck.complete())
            throw std::runtime_error("Console smoke scenario did not complete");
        std::cout << "Shutdown clean. Simulation ticks=" << tick << ", rendered frames=" << rendered.load()
                  << ", validation errors=" << validationErrors << "\n";
        return validationErrors ? 2 : 0;
    } catch (const std::exception& e) {
        std::cerr << "[Fatal] " << e.what() << "\n";
        return 1;
    }
}
