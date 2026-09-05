#include "core/World.h"
#include "core/FrameMailbox.h"
#include "scripting/ScriptRuntime.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
using namespace afterlight;
int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    try {
        RenderOptions options;
        uint32_t width = 1280, height = 800;
        int debugView = 0;
        bool demo = false, smoke = false;
        bool physicsDebug = false;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            auto number = [&]() {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing value for " + arg);
                return std::stoi(argv[++i]);
            };
            if (arg == "--frames")
                options.maxFrames = uint32_t(std::max(1, number()));
            else if (arg == "--width")
                width = uint32_t(std::clamp(number(), 640, 2560));
            else if (arg == "--height")
                height = uint32_t(std::clamp(number(), 400, 1440));
            else if (arg == "--view")
                debugView = std::clamp(number(), 0, 7);
            else if (arg == "--capture")
                options.capture = true;
            else if (arg == "--no-hud")
                options.hud = false;
            else if (arg == "--physics-debug")
                physicsDebug = true;
            else if (arg == "--audit") {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing audit name");
                options.audit = argv[++i];
                if (options.audit.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                    std::string::npos)
                    throw std::runtime_error("Audit name must be a simple directory name");
            } else if (arg == "--audit-motion")
                options.auditMotion = true;
            else if (arg == "--no-validation")
                options.validation = false;
            else if (arg == "--validation")
                options.validation = true;
            else if (arg == "--present") {
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
            } else if (arg == "--demo")
                demo = true;
            else if (arg == "--smoke")
                smoke = true;
            else if (arg == "--help") {
                std::cout << "Afterlight [--frames N] [--capture] [--width 1280] [--height 800] [--view "
                             "0..7] [--no-hud] [--validation] [--no-validation] [--present "
                             "fifo|mailbox|immediate] [--demo] [--smoke] [--audit NAME] "
                             "[--audit-motion] [--physics-debug]\n";
                return 0;
            } else
                throw std::runtime_error("Unknown option " + arg);
        }
        if (options.auditMotion && options.audit.empty())
            throw std::runtime_error("--audit-motion requires --audit NAME");
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
        if (smoke) {
            options.maxFrames = 160;
            options.capture = true;
            // The smoke scenario is the project's correctness gate, so it always pays for
            // validation regardless of the build configuration default.
            options.validation = true;
        }
        std::cout << "AFTERLIGHT | C++ engine / JavaScript gameplay / Vulkan RT\n";
        Window window(width, height);
        World world;
        ScriptRuntime scripts(world);
        scripts.initialize();
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
        std::atomic<double> renderFps{-1}, renderFrameMs{0}, renderGpuMs{0};
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
                    rendered.store(renderer.frames());
                    const auto stats = renderer.statistics();
                    renderFps.store(stats.fps);
                    renderFrameMs.store(stats.frameMs);
                    renderGpuMs.store(stats.gpuMs);
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
        auto restoreAt = Clock::time_point::max();
        mailbox.publish(world.snapshot(window.input(), tick, time, debugView, physicsDebug));
        try {
            while (window.pump() && !finished.load()) {
                auto now = Clock::now();
                accumulator += std::min(std::chrono::duration<double>(now - previous).count(), .1);
                previous = now;
                bool changed = false;
                while (accumulator >= step) {
                    Input input = window.input();
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
                        }
                    }
                    if (input.pressed[VK_F1])
                        debugView = (debugView + 1) % 8;
                    if (input.pressed[VK_F2])
                        physicsDebug = !physicsDebug;
                    if (options.audit.empty())
                        scripts.tick(float(step), input);
                    window.consumeEdges();
                    accumulator -= step;
                    if (options.audit.empty())
                        time += step;
                    tick++;
                    changed = true;
                }
                if (changed)
                    mailbox.publish(world.snapshot(window.input(), tick, time, debugView, physicsDebug));
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
                              << std::setprecision(2) << "Frame " << renderFrameMs.load() << " ms | GPU "
                              << renderGpuMs.load() << " ms | 60 FPS target / VSync ON";
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
        if (!mainError.empty())
            throw std::runtime_error(mainError);
        if (!renderError.empty())
            throw std::runtime_error(renderError);
        if (smoke && smokeStage != 6)
            throw std::runtime_error("Smoke scenario did not complete");
        std::cout << "Shutdown clean. Simulation ticks=" << tick << ", rendered frames=" << rendered.load()
                  << ", validation errors=" << validationErrors << "\n";
        return validationErrors ? 2 : 0;
    } catch (const std::exception& e) {
        std::cerr << "[Fatal] " << e.what() << "\n";
        return 1;
    }
}
