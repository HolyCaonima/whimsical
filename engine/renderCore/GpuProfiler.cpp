#include "renderCore/GpuProfiler.h"
#include <algorithm>
namespace whimsical {
GpuProfiler::GpuProfiler(VulkanContext& vk) : vk_(vk) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, queues.data());
    validBits_ = queues.at(vk.family).timestampValidBits;
    if (validBits_)
        pools_.push_back(allocatePool());
}
VkQueryPool GpuProfiler::allocatePool() {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = ScopesPerPool * 2;
    VkQueryPool pool;
    VK_CHECK(vkCreateQueryPool(vk_.device, &info, nullptr, &pool));
    return pool;
}
GpuProfiler::~GpuProfiler() {
    for (auto pool : pools_)
        vkDestroyQueryPool(vk_.device, pool, nullptr);
}
void GpuProfiler::timestamp(VkCommandBuffer command, uint32_t query, VkPipelineStageFlags2 stage) {
    const uint32_t page = query / (ScopesPerPool * 2);
    if (page == pools_.size()) {
        pools_.push_back(allocatePool());
        vkCmdResetQueryPool(command, pools_.back(), 0, ScopesPerPool * 2);
    }
    vkCmdWriteTimestamp2(command, stage, pools_.at(page), query % (ScopesPerPool * 2));
}
void GpuProfiler::beginFrame(VkCommandBuffer command, uint64_t request, uint64_t frame, uint32_t width,
                             uint32_t height) {
    recording_ = {request, frame, width, height, vk_.properties.deviceName, {}, {}};
    stack_.clear();
    detailed_ = request != 0;
    if (pools_.empty()) {
        if (request) {
            recording_.error = "The selected Vulkan queue does not support GPU timestamps.";
            completed_ = recording_;
        }
        return;
    }
    if (detailed_)
        for (auto pool : pools_)
            vkCmdResetQueryPool(command, pool, 0, ScopesPerPool * 2);
    else
        vkCmdResetQueryPool(command, pools_.front(), 0, 2);
    recording_.scopes.push_back({"GPU Frame", -1, 0});
    stack_.push_back(0);
    timestamp(command, 0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
}
int GpuProfiler::beginScope(VkCommandBuffer command, const char* name) {
    if (pools_.empty() || !detailed_)
        return -1;
    int index = int(recording_.scopes.size());
    recording_.scopes.push_back({name, stack_.back(), 0});
    stack_.push_back(index);
    // Same-stage boundaries keep nested intervals ordered without adding pipeline barriers.
    timestamp(command, uint32_t(index) * 2, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    return index;
}
void GpuProfiler::endScope(VkCommandBuffer command, int index) {
    if (index < 0)
        return;
    timestamp(command, uint32_t(index) * 2 + 1, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    stack_.pop_back();
}
void GpuProfiler::endFrame(VkCommandBuffer command) {
    if (pools_.empty())
        return;
    timestamp(command, 1, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    stack_.clear();
    pending_ = true;
}
void GpuProfiler::resolve() {
    if (!pending_)
        return;
    const auto count = uint32_t(recording_.scopes.size()) * 2;
    std::vector<uint64_t> values(count);
    for (uint32_t first = 0; first < count; first += ScopesPerPool * 2) {
        const auto n = std::min(count - first, ScopesPerPool * 2);
        VK_CHECK(vkGetQueryPoolResults(vk_.device, pools_[first / (ScopesPerPool * 2)], 0, n,
                                       size_t(n) * sizeof(uint64_t), values.data() + first,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
    }
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
