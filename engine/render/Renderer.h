#pragma once
#include "core/Types.h"
#include <windows.h>
#include <memory>
namespace afterlight {
struct RenderOptions {
    bool validation = true;
    bool capture = false;
    uint32_t maxFrames = 0;
    bool hud = true;
    std::string audit;
    bool auditMotion = false;
};
struct RenderStatistics {
    double fps = -1;
    double frameMs = 0;
    double gpuMs = 0;
};
class Renderer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Renderer(HWND, const RenderOptions&);
    ~Renderer();
    bool render(const Frame&);
    uint64_t frames() const;
    RenderStatistics statistics() const;
    uint32_t errors() const;
};
} // namespace afterlight
