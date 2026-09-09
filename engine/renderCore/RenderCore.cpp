#include "RenderCore.h"
#include "ShaderCompiler.h"
#include "GpuProfiler.h"
#include "graph/RenderGraph.h"
#include "graph/ImageReadback.h"
#include "vulkan/VulkanAccess.h"
#include "vulkan/ProgramStorage.h"
#include <cstring>
#include <algorithm>

namespace whimsical::rc {
struct RenderCore::Impl {
    VulkanContext vk;
};
RenderCore::RenderCore(const DeviceOptions& options) : impl_(std::make_unique<Impl>()) {
    impl_->vk.initialize(static_cast<HWND>(options.presentationWindow), options.validation, options.rayQueries);
}
RenderCore::~RenderCore() = default;
void RenderCore::waitIdle() { VK_CHECK(vkDeviceWaitIdle(impl_->vk.device)); }
uint32_t RenderCore::errors() const { return impl_->vk.validationErrors.load(); }
VulkanContext& VulkanAccess::device(RenderCore& core) { return core.impl_->vk; }

struct GraphContext::Impl {
    VulkanContext& vk;
    rg::ResourcePool pool;
    rg::RenderGraph graph;
    GpuProfiler profiler;
    ShaderCompiler compiler;
    std::map<std::vector<uint32_t>, rg::Program> programs;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false, recorded = false, completed = false;

    Impl(RenderCore& core, const rg::Registry& registry)
        : vk(VulkanAccess::device(core)), pool(vk, registry), graph(pool), profiler(vk) {
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(vk.device, &info, nullptr, &fence));
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = vk.commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        auto result = vkAllocateCommandBuffers(vk.device, &allocation, &command);
        if (result != VK_SUCCESS) {
            vkDestroyFence(vk.device, fence, nullptr);
            VK_CHECK(result);
        }
    }
    ~Impl() {
        // Resource owners may disappear while work is in flight. Reclaim this scope only
        // after its own submission, without stalling unrelated contexts with DeviceWaitIdle.
        if (pending)
            vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &command);
        vkDestroyFence(vk.device, fence, nullptr);
    }
    void writable() const {
        if (pending)
            throw std::logic_error("Complete this graph's submission before changing its resources or passes");
    }
    void submit(const SubmissionSync& sync) {
        writable();
        if (!recorded)
            throw std::logic_error("Record a graph before submitting it");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        if (sync.wait) {
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &sync.wait;
            info.pWaitDstStageMask = &sync.waitStage;
        }
        if (sync.signal) {
            info.signalSemaphoreCount = 1;
            info.pSignalSemaphores = &sync.signal;
        }
        VK_CHECK(vkResetFences(vk.device, 1, &fence));
        VK_CHECK(vkQueueSubmit(vk.queue, 1, &info, fence));
        pending = true;
        recorded = false;
    }
};
GraphContext::GraphContext(RenderCore& core, const rg::Registry& registry)
    : impl_(std::make_unique<Impl>(core, registry)) {}
GraphContext::~GraphContext() = default;
rg::RenderGraph& GraphContext::graph() {
    impl_->writable();
    return impl_->graph;
}
const rg::Program& GraphContext::compute(const std::vector<uint32_t>& code) {
    impl_->writable();
    auto found = impl_->programs.find(code);
    if (found != impl_->programs.end())
        return found->second;
    rg::Program program;
    auto storage = std::make_shared<rg::ProgramStorage>(impl_->vk);
    program.accesses = rg::reflect(impl_->pool.registry(), code.data(), code.size());
    program.localSize = rg::reflectLocalSize(code.data(), code.size());
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = code.size() * sizeof(uint32_t);
    moduleInfo.pCode = code.data();
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(impl_->vk.device, &moduleInfo, nullptr, &module));
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.layout = impl_->pool.pipelineLayout();
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    auto result = vkCreateComputePipelines(impl_->vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                           &storage->pipeline);
    vkDestroyShaderModule(impl_->vk.device, module, nullptr);
    VK_CHECK(result);
    program.storage = std::move(storage);
    return impl_->programs.emplace(code, std::move(program)).first->second;
}
const rg::Program& GraphContext::compute(const std::string& name, const std::string& source) {
    return compute(impl_->compiler.compile(name, source));
}
void GraphContext::upload(rg::ResourceRef target, std::vector<uint8_t> bytes) {
    impl_->writable();
    if (impl_->pool.registry()[target.id].kind != rg::Kind::Buffer || bytes.empty() || bytes.size() % 4)
        throw std::invalid_argument("Upload requires a buffer and complete four-byte-aligned shader data");
    impl_->graph.add("Upload " + impl_->pool.registry()[target.id].name)
        .overwrite(target, rg::Access::Transfer)
        .record([target, bytes = std::move(bytes)](const rg::PassContext& context) {
            const auto& buffer = context.buffer(target);
            if (buffer.size != bytes.size())
                throw std::invalid_argument("Upload payload must cover the whole destination buffer");
            // vkCmdUpdateBuffer copies CPU data into command storage during recording.
            // Splitting at 64 KiB keeps the lifetime inside the recorded submission.
            for (size_t offset = 0; offset < bytes.size(); offset += 65536) {
                const auto size = std::min(size_t(65536), bytes.size() - offset);
                vkCmdUpdateBuffer(context.command, buffer.handle, offset, size, bytes.data() + offset);
            }
        });
}
void GraphContext::readback(rg::ResourceRef source, rg::ResourceId destination) {
    impl_->writable();
    const auto& registry = impl_->pool.registry();
    if (registry[destination].kind != rg::Kind::Buffer || !rg::hostRead(registry[destination]))
        throw std::invalid_argument("Readback destination must be a buffer with Host handover");
    if (registry[source.id].kind == rg::Kind::Image) {
        rg::addImageReadback(impl_->graph, "Image readback", destination, {{source}});
        return;
    }
    if (registry[source.id].kind != rg::Kind::Buffer)
        throw std::invalid_argument("Readback source must be a buffer or image");
    impl_->graph.add("Readback " + registry[source.id].name)
        .read(source, rg::Access::Transfer)
        .overwrite(destination, rg::Access::Transfer)
        .record([source, destination](const rg::PassContext& context) {
            const auto& input = context.buffer(source);
            const auto& output = context.buffer(destination);
            if (input.size != output.size)
                throw std::invalid_argument("Readback source and destination byte sizes must match");
            VkBufferCopy region{0, 0, input.size};
            vkCmdCopyBuffer(context.command, input.handle, output.handle, 1, &region);
        });
}
std::vector<uint8_t> GraphContext::readbackData(rg::ResourceId id) const {
    impl_->writable();
    if (!impl_->completed)
        throw std::logic_error("Readback data requires a completed submission");
    if (!rg::hostRead(impl_->pool.registry()[id]))
        throw std::invalid_argument("Readback data requires a Host handover resource");
    const auto& buffer = impl_->pool.buffer(id);
    if (!buffer.mapped)
        throw std::logic_error("Readback resource has not been produced");
    const auto* bytes = static_cast<const uint8_t*>(buffer.mapped);
    return {bytes, bytes + buffer.size};
}
void GraphContext::compile(uint32_t width, uint32_t height) {
    impl_->writable();
    impl_->graph.compile(width, height);
}
void GraphContext::record(const ProfileRequest& profile) {
    impl_->writable();
    impl_->recorded = impl_->completed = false;
    VK_CHECK(vkResetCommandBuffer(impl_->command, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(impl_->command, &begin));
    impl_->profiler.beginFrame(impl_->command, profile.request, profile.sequence, profile.width, profile.height);
    rg::GraphAccess::execute(impl_->graph, impl_->command, impl_->profiler);
    impl_->profiler.endFrame(impl_->command);
    VK_CHECK(vkEndCommandBuffer(impl_->command));
    impl_->recorded = true;
}
void GraphContext::submit() { impl_->submit({}); }
bool GraphContext::poll() {
    if (!impl_->pending)
        return true;
    auto status = vkGetFenceStatus(impl_->vk.device, impl_->fence);
    if (status == VK_NOT_READY)
        return false;
    VK_CHECK(status);
    impl_->profiler.resolve();
    impl_->pending = false;
    impl_->completed = true;
    return true;
}
void GraphContext::wait() {
    if (!impl_->pending)
        return;
    VK_CHECK(vkWaitForFences(impl_->vk.device, 1, &impl_->fence, VK_TRUE, UINT64_MAX));
    impl_->profiler.resolve();
    impl_->pending = false;
    impl_->completed = true;
}
void GraphContext::advanceHistory() { impl_->pool.flip(); }
double GraphContext::gpuMilliseconds() const { return impl_->profiler.frameMs(); }
std::optional<GpuProfile> GraphContext::takeProfile() { return impl_->profiler.takeResult(); }
rg::ResourcePool& VulkanAccess::resources(GraphContext& context) { return context.impl_->pool; }
GpuProfiler& VulkanAccess::profiler(GraphContext& context) { return context.impl_->profiler; }
void VulkanAccess::submit(GraphContext& context, const SubmissionSync& sync) { context.impl_->submit(sync); }
} // namespace whimsical::rc
