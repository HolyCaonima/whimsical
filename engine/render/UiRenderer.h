#pragma once
#include "renderCore/vulkan/VulkanContext.h"
#include "uiCore/UiFrame.h"
#include <memory>
namespace whimsical {
// Render-thread-only backend. No RmlUi context or JS pointers cross this boundary.
class UiRenderer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit UiRenderer(VulkanContext&);
    ~UiRenderer();
    // Called after the frame fence. Geometry occupies reusable ranges in retained
    // buffers; retire ranges only once no CPU snapshot/context holds the geometry.
    void prepare(const ui::UiFrame*);
    // Called inside a render graph pass that has already declared the target attachment.
    void draw(VkCommandBuffer, uint32_t width, uint32_t height, const ui::UiFrame*);
};
} // namespace whimsical
