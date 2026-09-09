#pragma once
#include "renderCore/vulkan/VulkanContext.h"
#include "core/Types.h"
#include <NRD.h>
#include <array>
namespace whimsical {
class GpuProfiler;
class NrdDenoiser {
    VulkanContext& vk_;
    nrd::Instance* instance_ = nullptr;
    const nrd::InstanceDesc* desc_ = nullptr;
    struct Pipeline {
        VkDescriptorSetLayout resources = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline handle = VK_NULL_HANDLE;
    };
    std::vector<Pipeline> pipelines_;
    std::vector<Image> permanent_, transient_;
    std::array<VkSampler, 2> samplers_{};
    VkDescriptorSetLayout constantsLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    Buffer constants_;
    uint32_t width_ = 0, height_ = 0;
    mat4 previousView_{1}, previousProjection_{1};
    bool first_ = true;

  public:
    explicit NrdDenoiser(VulkanContext&);
    ~NrdDenoiser();
    void resize(uint32_t, uint32_t);
    void dispatch(VkCommandBuffer, const std::array<Image*, size_t(nrd::ResourceType::MAX_NUM)>&,
                  const Camera&, uint32_t frame, bool reset, float frameMs, GpuProfiler&);
};
} // namespace whimsical
