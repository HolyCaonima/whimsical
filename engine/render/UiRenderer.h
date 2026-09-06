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
    void draw(VkCommandBuffer, Image& target, const ui::UiFrame*);
};
} // namespace afterlight
