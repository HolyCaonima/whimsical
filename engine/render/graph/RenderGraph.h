#pragma once
#include "ResourcePool.h"
#include "render/GpuProfiler.h"
#include <functional>

namespace afterlight::rg {
class RenderGraph;

// Everything a pass body is allowed to know: a command buffer, the descriptor set for this
// frame's physical mapping, and resolved handles for the resources it declared. Resolving
// a resource the pass did not declare throws, which is what ties the GPU work actually
// recorded to the access declarations the compiler synchronised against.
struct PassContext {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    ResourcePool* pool = nullptr;
    const RenderGraph* graph = nullptr;
    uint32_t pass = 0;
    Image& image(ResourceRef) const;
    Buffer& buffer(ResourceRef) const;
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
        // The three things a pass can do to contents, and the only three. Consuming a
        // resource nothing in this frame produced is legal exactly when its lifetime says
        // the contents came from an earlier frame or from its owner.
        Builder& use(ResourceRef, Access, Usage);
        Builder& read(ResourceRef, Access);
        Builder& read(const ResourceList&, Access);
        Builder& overwrite(ResourceRef, Access);
        Builder& overwrite(const ResourceList&, Access);
        Builder& modify(ResourceRef, Access);
        Builder& modify(const ResourceList&, Access);
        // Render targets. The graph owns vkCmdBeginRendering so no pass repeats attachment
        // setup, and the load op follows from the declaration: a cleared attachment
        // overwrites, a loaded one modifies and therefore needs a producer.
        Builder& color(ResourceId);
        Builder& color(ResourceId, VkClearColorValue clear);
        Builder& depth(ResourceId, float clear = 1.f);
        // The common case: bind the pipeline and the descriptor set, dispatch over the
        // resource extent. divisor matches the declared divisor of the pass output.
        Builder& dispatch(VkPipeline, uint16_t divisor = 1);
        Builder& record(std::function<void(const PassContext&)>);
        // The pass changes state the graph does not own — a denoiser's own accumulation,
        // for instance — so it survives culling on its own account. It is not a way to
        // keep a pass alive whose product the graph can already see.
        Builder& sideEffect();
    };

    explicit RenderGraph(ResourcePool&);
    // Declarations alone. compile() is a pure function of them, so cull, validation,
    // liveness and storage reuse can be exercised without a device.
    explicit RenderGraph(const Registry&);
    void reset();
    Builder add(const char* name);
    // Resolve content versions into dependencies, cull, decide what storage the frame
    // needs and make the pool match.
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
    bool alive(uint32_t pass) const {
        return passes_[pass].alive;
    }
    const std::vector<Residency>& residency() const {
        return residency_;
    }
    // Throws unless the pass declared this reference. PassContext is its only caller.
    void checkDeclared(uint32_t pass, ResourceRef) const;

  private:
    static constexpr uint32_t NoPass = 0xffffffff;
    // One declared access, plus what the compiler resolved it against: the pass whose
    // version of the contents this one consumes, and whether the version it produces is
    // consumed again before something replaces it.
    struct Use {
        ResourceRef ref;
        Access access = Access::Compute;
        Usage usage = Usage::Read;
        uint32_t source = NoPass;
        bool consumedLater = false;
    };
    struct Attachment {
        ResourceId id;
        uint32_t use = 0;
        bool clear = false;
        VkClearValue value{};
    };
    struct Pass {
        const char* name = "";
        uint32_t first = 0, count = 0; // slice of uses_
        uint32_t firstColor = 0, colorCount = 0;
        uint32_t firstEdge = 0, edgeCount = 0; // slice of edges_
        ResourceId depth;
        uint32_t depthUse = 0;
        float depthClear = 1.f;
        VkPipeline pipeline = VK_NULL_HANDLE;
        uint16_t divisor = 1;
        bool sideEffect = false;
        bool alive = true;
        std::function<void(const PassContext&)> record;
    };
    // One physical slot's worth of a pass's declarations, after the several ways a pass can
    // name the same storage have been folded together.
    struct Merged {
        ResourceRef ref;
        uint32_t slot = 0;
        AccessInfo info;
        bool produces = false;
    };
    const Registry& registry_;
    ResourcePool* pool_ = nullptr;
    std::vector<Pass> passes_;
    std::vector<Use> uses_;
    std::vector<Attachment> colors_;
    std::vector<uint32_t> edges_;    // consumer -> producing pass, in pass order
    std::vector<uint32_t> producer_; // content slot -> pass that produced the live version
    std::vector<uint8_t> touched_;   // resource -> a live pass names it this frame
    std::vector<Residency> residency_;
    std::vector<Use> handover_;
    std::vector<Merged> merged_;
    std::vector<VkRenderingAttachmentInfo> attachments_;
    std::vector<VkImageMemoryBarrier2> imageBarriers_;
    std::vector<VkBufferMemoryBarrier2> bufferBarriers_;
    std::vector<VkMemoryBarrier2> memoryBarriers_;
    uint32_t live_ = 0, barriers_ = 0, aliased_ = 0;

    uint32_t content(ResourceRef ref) const {
        return uint32_t(ref.id.index) * 2 + (ref.slot == Slot::Previous);
    }
    void analyse();
    void cull();
    void validate() const;
    void liveness();
    void plan(uint32_t width, uint32_t height);
    void synchronise(VkCommandBuffer, const Use*, uint32_t count);
    void handover(VkCommandBuffer);
};
} // namespace afterlight::rg
