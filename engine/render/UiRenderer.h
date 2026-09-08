#pragma once
#include "VulkanContext.h"
#include "uiCore/UiFrame.h"
#include <memory>
namespace afterlight {
// Render-thread-only backend. No RmlUi context or JS pointers cross this boundary.
class UiRenderer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit UiRenderer(VulkanContext&);
    ~UiRenderer();
    // Called after the frame fence; upload resources and retire those no snapshot retains.
    void prepare(const ui::UiFrame*);
    // Called inside a render graph pass that has already declared the target attachment.
    void draw(VkCommandBuffer, uint32_t width, uint32_t height, const ui::UiFrame*);
};
} // namespace afterlight
