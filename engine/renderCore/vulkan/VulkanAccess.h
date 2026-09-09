#pragma once
#include "renderCore/RenderCore.h"
#include "VulkanContext.h"
#include "renderCore/graph/GraphTypes.h"

namespace whimsical {
namespace rg {
class ResourcePool;
}
namespace rc {
// Backend extension for native raster/AS/third-party SDK integration. Ordinary compute
// contributors use GraphContext and graph builders, without needing this header.
struct SubmissionSync {
    VkSemaphore wait = VK_NULL_HANDLE;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore signal = VK_NULL_HANDLE;
};
struct VulkanAccess {
    static VulkanContext& device(RenderCore&);
    static void submit(GraphContext&, const SubmissionSync&);
};
// Native imports are scoped to an execution context. Mutation uses the same
// completion gate as graph rebuilding; allocation, barriers and history stay private.
class NativeResources {
    GraphContext& context_;

  public:
    explicit NativeResources(GraphContext& context) : context_(context) {}
    void importImage(rg::ResourceId, Image&);
    void importImage(rg::ResourceId, Image&, rg::AccessState&);
    void importBuffer(rg::ResourceId, Buffer&);
    void importBuffer(rg::ResourceId, Buffer&, rg::AccessState&, std::shared_ptr<void> owner = {});
    void clearImport(rg::ResourceId);
    rg::AccessState bufferState(rg::ResourceRef) const;
    void importTlas(rg::ResourceId, VkAccelerationStructureKHR, uint64_t generation);
    void importSamplers(rg::ResourceId, std::vector<VkDescriptorImageInfo>);
    void syncImports();
    const rg::Registry& registry() const;
    VkPipelineLayout pipelineLayout() const;
    const Buffer& buffer(rg::ResourceRef) const;
    uint64_t ownedBytes() const;
    uint64_t declaredBytes() const;
    uint64_t descriptorWrites() const;
};
} // namespace rc
} // namespace whimsical
