#pragma once
#include "VulkanContext.h"
#include <functional>
#include <vector>
#include <string>
namespace afterlight {
// Explicitly ordered graph for the first renderer. Every edge is synchronized.
// Images declare layouts at their owning pass; shared buffers use graph barriers.
class RenderGraph {
    struct Pass {
        std::string name;
        std::function<void(VkCommandBuffer)> record;
    };
    std::vector<Pass> passes_;

  public:
    void add(std::string name, std::function<void(VkCommandBuffer)> record) {
        passes_.push_back({std::move(name), std::move(record)});
    }
    void execute(VkCommandBuffer command) const {
        for (auto& pass : passes_) {
            if (vkCmdBeginDebugUtilsLabelEXT) {
                VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
                label.pLabelName = pass.name.c_str();
                vkCmdBeginDebugUtilsLabelEXT(command, &label);
            }
            pass.record(command);
            VulkanContext::barrier(command);
            if (vkCmdEndDebugUtilsLabelEXT)
                vkCmdEndDebugUtilsLabelEXT(command);
        }
    }
};
} // namespace afterlight
