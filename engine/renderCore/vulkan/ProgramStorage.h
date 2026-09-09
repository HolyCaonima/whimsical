#pragma once
#include "VulkanContext.h"

namespace whimsical::rg {
// Native program lifetime stays behind the public, backend-independent program handle.
struct ProgramStorage {
    VulkanContext& vk;
    VkPipeline pipeline = VK_NULL_HANDLE;
    explicit ProgramStorage(VulkanContext& device) : vk(device) {}
    ~ProgramStorage() {
        if (pipeline)
            vkDestroyPipeline(vk.device, pipeline, nullptr);
    }
};
} // namespace whimsical::rg
