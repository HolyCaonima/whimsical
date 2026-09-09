#pragma once
#include "renderCore/RenderCore.h"
#include "VulkanContext.h"

namespace whimsical {
namespace rg { class ResourcePool; }
namespace rc {
// Backend extension for native raster/AS/third-party SDK integration. Ordinary compute
// contributors use GraphContext and graph builders, without needing this header.
struct SubmissionSync {
    VkSemaphore wait = VK_NULL_HANDLE;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore signal = VK_NULL_HANDLE;
};
struct VulkanAccess {
    static VulkanContext& device(RenderCore&);
    static rg::ResourcePool& resources(GraphContext&);
    static GpuProfiler& profiler(GraphContext&);
    static void submit(GraphContext&, const SubmissionSync&);
};
} // namespace rc
} // namespace whimsical
