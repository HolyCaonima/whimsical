#pragma once
#include "core/CpuProfile.h"
#include "VulkanContext.h"
#include "GpuProfiler.h"
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
    void execute(VkCommandBuffer command, GpuProfiler& profiler) const {
        for (auto& pass : passes_) {
            CpuScope cpuScope(pass.name.c_str());
            GpuScope scope(profiler, command, pass.name.c_str());
            pass.record(command);
            VulkanContext::barrier(command);
        }
    }
};
} // namespace afterlight
