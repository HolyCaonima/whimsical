#pragma once
#include "renderCore/vulkan/VulkanContext.h"
#include "core/GpuProfile.h"
#include <optional>
namespace whimsical {
// Owned by one graph execution context. Resolve only after its submission completes.
class GpuProfiler {
    static constexpr uint32_t ScopesPerPool = 256;
    VulkanContext& vk_;
    std::vector<VkQueryPool> pools_;
    uint32_t validBits_ = 0;
    bool pending_ = false, detailed_ = false;
    GpuProfile recording_;
    std::vector<int> stack_;
    std::optional<GpuProfile> completed_;
    double frameMs_ = 0;
    VkQueryPool allocatePool();
    void timestamp(VkCommandBuffer, uint32_t query, VkPipelineStageFlags2);

  public:
    explicit GpuProfiler(VulkanContext&);
    ~GpuProfiler();
    void beginFrame(VkCommandBuffer, uint64_t request, uint64_t frame, uint32_t width, uint32_t height);
    void endFrame(VkCommandBuffer);
    int beginScope(VkCommandBuffer, const char* name);
    void endScope(VkCommandBuffer, int scope);
    void resolve();
    double frameMs() const {
        return frameMs_;
    }
    std::optional<GpuProfile> takeResult();
    bool labelsEnabled() const {
        return vk_.debugLabels;
    }
};
class GpuScope {
    GpuProfiler& profiler_;
    VkCommandBuffer command_;
    int scope_;

  public:
    GpuScope(GpuProfiler&, VkCommandBuffer, const char* name);
    ~GpuScope();
    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;
};
} // namespace whimsical
