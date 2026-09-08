#pragma once
#include "ResourcePool.h"
#include "render/GpuProfiler.h"
#include <functional>

namespace afterlight::rg {
class RenderGraph;

// Everything a pass body is allowed to know: a command buffer, the descriptor set for this
// frame's physical mapping, and resolved handles for the resources it declared.
struct PassContext {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    ResourcePool* pool = nullptr;
    Image& image(ResourceRef ref) const {
        return pool->image(ref);
    }
    Buffer& buffer(ResourceRef ref) const {
        return pool->buffer(ref);
    }
};

// Declared once per frame by render features. The graph derives execution order, culling,
// barriers, layouts and transient storage reuse from these declarations alone.
class RenderGraph {
  public:
    class Builder {
        RenderGraph* graph_;
        uint32_t pass_;
        friend class RenderGraph;
        Builder(RenderGraph* graph, uint32_t pass) : graph_(graph), pass_(pass) {}

      public:
        Builder& read(ResourceRef, Access);
        Builder& read(const ResourceList&, Access);
        Builder& write(ResourceRef, Access);
        Builder& write(const ResourceList&, Access);
            // Render targets. Declaring one is also declaring the write, and the graph owns
            // vkCmdBeginRendering so no pass repeats attachment setup.
            Builder& color(ResourceId);
            Builder& color(ResourceId, VkClearColorValue clear);
            Builder& depth(ResourceId, float clear = 1.f);
        // The common case: bind the pipeline and the descriptor set, dispatch over the
        // resource extent. divisor matches the declared divisor of the pass output.
        Builder& dispatch(VkPipeline, uint16_t divisor = 1);
        Builder& record(std::function<void(const PassContext&)>);
        Builder& sideEffect();
    };

    explicit RenderGraph(ResourcePool&);
    void reset();
    Builder add(const char* name);
    // Cull, compute transient lifetimes, pick storage reuse and make the pool match.
    void compile(uint32_t width, uint32_t height);
    void execute(VkCommandBuffer, GpuProfiler&);

    uint32_t passCount() const {
        return uint32_t(passes_.size());
    }
    uint32_t livePasses() const {
        return live_;
    }
    uint32_t barrierCount() const {
        return barriers_;
    }
    uint32_t aliasedResources() const {
        return aliased_;
    }

  private:
    struct Use {
        ResourceRef ref;
        Access access = Access::None;
    };
    struct Attachment {
        ResourceId id;
        bool clear = false;
        VkClearValue value{};
    };
    struct Pass {
        const char* name = "";
        uint32_t first = 0, count = 0; // slice of uses_
        uint32_t firstColor = 0, colorCount = 0;
        ResourceId depth;
        float depthClear = 1.f;
        VkPipeline pipeline = VK_NULL_HANDLE;
        uint16_t divisor = 1;
        bool sideEffect = false;
        bool alive = true;
        std::function<void(const PassContext&)> record;
    };
    ResourcePool& pool_;
    std::vector<Pass> passes_;
    std::vector<Use> uses_;
    std::vector<Attachment> colors_;
    std::vector<ResourceId> aliasRoot_;
    std::vector<Use> merged_;
    std::vector<uint8_t> needed_;
    std::vector<VkRenderingAttachmentInfo> attachments_;
    std::vector<VkImageMemoryBarrier2> imageBarriers_;
    std::vector<VkBufferMemoryBarrier2> bufferBarriers_;
    std::vector<VkMemoryBarrier2> memoryBarriers_;
    uint32_t live_ = 0, barriers_ = 0, aliased_ = 0;

    void cull();
    void alias(uint32_t width, uint32_t height);
    void synchronise(VkCommandBuffer, const Pass&);
};
} // namespace afterlight::rg
