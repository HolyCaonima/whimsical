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
    PresentMode present = PresentMode::Fifo;
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
    uint32_t errors() const;
};
} // namespace afterlight
