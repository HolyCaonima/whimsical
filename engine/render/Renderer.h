#pragma once
#include "core/Types.h"
#include <windows.h>
#include <memory>
namespace afterlight {
// Swapchain pacing. FIFO is the shipping default; the uncapped modes exist so frame cost
// can be measured without the display clamping every result to a vblank multiple.
enum class PresentMode { Fifo, Mailbox, Immediate };
struct RenderOptions {
    // Validation costs roughly a millisecond of CPU per frame, which is enough to push an
    // otherwise on-budget frame past a vblank. Debug builds pay it; Release opts in.
#ifdef NDEBUG
    bool validation = false;
#else
    bool validation = true;
#endif
    bool capture = false;
    uint32_t maxFrames = 0;
    bool hud = true;
    std::string audit;
    bool auditMotion = false;
    // Forces the renderer to rewrite every slot every frame, which is what it did before
    // the scene became persistent. Kept so the two behaviours can be measured against each
    // other in one binary, where the only difference is this flag.
    bool fullUpload = false;
    PresentMode present = PresentMode::Fifo;
};
// What the last frame actually had to touch in the persistent scene. Exposed because
// "a scene that did not change costs nothing" is a property worth being able to assert.
struct SceneUpdateStatistics {
    uint32_t slots = 0;      // Slot capacity, i.e. how much a full rebuild would cost.
    uint32_t transforms = 0; // Slots whose transform half was rewritten.
    uint32_t attributes = 0; // Slots whose mesh/material/visibility half was rewritten.
    uint32_t structural = 0; // Slots created, destroyed or reused.
    uint32_t settled = 0;    // Slots whose motion was folded forward after they stopped.
    bool resynchronised = false; // A skipped snapshot forced a full rewrite.
    bool tlasRebuilt = false;    // Topology changed, so no incremental update was possible.
};
struct RenderStatistics {
    double fps = -1;
    double frameMs = 0;
    double gpuMs = 0;
    // Static string literal owned by the renderer; safe to copy by pointer.
    const char* present = "FIFO";
    bool vsync = true;
};
class Renderer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Renderer(HWND, const RenderOptions&);
    ~Renderer();
    // Takes a reference-counted snapshot so the renderer can retain the previous frame
    // for motion vectors without copying it.
    bool render(const FrameRef&);
    uint64_t frames() const;
    RenderStatistics statistics() const;
    SceneUpdateStatistics sceneStatistics() const;
    uint32_t errors() const;
};
} // namespace afterlight
