#include "renderCore/GpuProfiler.h"
namespace whimsical {
GpuProfiler::GpuProfiler(VulkanContext& vk) : vk_(vk) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, queues.data());
    validBits_ = queues.at(vk.family).timestampValidBits;
    if (validBits_) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = MaxScopes * 2;
        VK_CHECK(vkCreateQueryPool(vk.device, &info, nullptr, &pool_));
    }
}
GpuProfiler::~GpuProfiler() {
    if (pool_)
        vkDestroyQueryPool(vk_.device, pool_, nullptr);
}
void GpuProfiler::beginFrame(VkCommandBuffer command, uint64_t request, uint64_t frame, uint32_t width,
                             uint32_t height) {
    recording_ = {request, frame, width, height, vk_.properties.deviceName, {}, {}};
    stack_.clear();
    detailed_ = request != 0;
    if (!pool_) {
        if (request) {
            recording_.error = "The selected Vulkan queue does not support GPU timestamps.";
            completed_ = recording_;
        }
        return;
    }
    vkCmdResetQueryPool(command, pool_, 0, detailed_ ? MaxScopes * 2 : 2);
    recording_.scopes.push_back({"GPU Frame", -1, 0});
    stack_.push_back(0);
    vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool_, 0);
}
int GpuProfiler::beginScope(VkCommandBuffer command, const char* name) {
    if (!pool_ || !detailed_)
        return -1;
    if (recording_.scopes.size() == MaxScopes) {
        recording_.error = "GPU scope capacity exceeded; report is incomplete.";
        return -1;
    }
    int index = int(recording_.scopes.size());
    recording_.scopes.push_back({name, stack_.back(), 0});
    stack_.push_back(index);
    // Same-stage boundaries keep nested intervals ordered without adding pipeline barriers.
    vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, pool_, uint32_t(index) * 2);
    return index;
}
void GpuProfiler::endScope(VkCommandBuffer command, int index) {
    if (index < 0)
        return;
    vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, pool_, uint32_t(index) * 2 + 1);
    stack_.pop_back();
}
void GpuProfiler::endFrame(VkCommandBuffer command) {
    if (!pool_)
        return;
    vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, pool_, 1);
    stack_.clear();
    pending_ = true;
}
void GpuProfiler::resolve() {
    if (!pending_)
        return;
    const auto count = uint32_t(recording_.scopes.size()) * 2;
    std::vector<uint64_t> values(count);
    VK_CHECK(vkGetQueryPoolResults(vk_.device, pool_, 0, count, values.size() * sizeof(uint64_t),
                                   values.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
    for (size_t i = 0; i < recording_.scopes.size(); ++i)
        recording_.scopes[i].milliseconds = gpuTimestampMilliseconds(
            values[i * 2], values[i * 2 + 1], validBits_, vk_.properties.limits.timestampPeriod);
    frameMs_ = recording_.scopes.front().milliseconds;
    if (recording_.request)
        completed_ = std::move(recording_);
    pending_ = false;
}
std::optional<GpuProfile> GpuProfiler::takeResult() {
    auto result = std::move(completed_);
    completed_.reset();
    return result;
}
GpuScope::GpuScope(GpuProfiler& profiler, VkCommandBuffer command, const char* name)
    : profiler_(profiler), command_(command), scope_(profiler.beginScope(command, name)) {
    if (profiler.labelsEnabled()) {
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = name;
        vkCmdBeginDebugUtilsLabelEXT(command, &label);
    }
}
GpuScope::~GpuScope() {
    if (profiler_.labelsEnabled())
        vkCmdEndDebugUtilsLabelEXT(command_);
    profiler_.endScope(command_, scope_);
}
} // namespace whimsical
