#pragma once
#include "renderCore/graph/RenderGraph.h"
#include "renderCore/graph/ResourcePool.h"
#include "renderCore/GpuProfiler.h"

namespace whimsical::rg {
// Native pass integration is opt-in. Resolve only resources declared by the pass, so
// backend commands and the graph's synchronization describe the same work.
struct PassContext {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    Image& image(ResourceRef) const;
    Buffer& buffer(ResourceRef) const;
    const Declaration& declaration(ResourceId) const;
    GpuProfiler& profiler() const { return *profiler_; }
  private:
    friend class RenderGraph;
    ResourcePool* pool = nullptr;
    const RenderGraph* graph = nullptr;
    uint32_t pass = 0;
    GpuProfiler* profiler_ = nullptr;
};
struct GraphAccess {
    static void execute(RenderGraph&, VkCommandBuffer, GpuProfiler&);
};
} // namespace whimsical::rg
