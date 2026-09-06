#pragma once
#include "VulkanContext.h"
#include "core/GpuProfile.h"
#include <optional>
namespace afterlight {
// Owned by the render thread. Resolve only after the submitted frame fence has completed.
class GpuProfiler {
    static constexpr uint32_t MaxScopes = 256;
    VulkanContext& vk_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    uint32_t validBits_ = 0;
    bool pending_ = false, detailed_ = false;
    GpuProfile recording_;
    std::vector<int> stack_;
    std::optional<GpuProfile> completed_;
    double frameMs_ = 0;

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
} // namespace afterlight
