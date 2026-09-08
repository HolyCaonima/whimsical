#pragma once
#include "Registry.h"
#include "render/VulkanContext.h"

// Vulkan vocabulary of the render graph. A pass names where it touches a resource and what
// it does to the contents; this file is the only place that turns that pair into the
// stage/access/layout triple a barrier is made of.
namespace afterlight::rg {
struct AccessInfo {
    VkPipelineStageFlags2 stage = 0;
    VkAccessFlags2 access = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
AccessInfo accessInfo(Access, Usage);

// Per physical resource, carried across frames so the first access of frame N is correctly
// ordered against the last access of frame N-1. Ping-ponged history and aliased transients
// depend on that being true. Image layout stays in Image::layout, its only home.
struct AccessState {
    VkPipelineStageFlags2 writeStage = 0;
    VkAccessFlags2 writeAccess = 0;
    VkPipelineStageFlags2 readStages = 0;    // reads since the last write, for write-after-read
    VkPipelineStageFlags2 flushedStages = 0; // reads already made visible to the last write
    VkAccessFlags2 flushedAccess = 0;
};
} // namespace afterlight::rg
