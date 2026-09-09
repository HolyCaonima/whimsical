#pragma once
#include "renderCore/vulkan/GraphAccess.h"
#include "renderCore/vulkan/ProgramStorage.h"

namespace whimsical::rg {
struct RenderGraph::Impl {
    RenderGraph& owner;
    Impl(RenderGraph& graph, const Registry& registry, ResourcePool* pool)
        : owner(graph), registry_(registry), pool_(pool) {}
    void reset();
    void compile(uint32_t, uint32_t);
    void execute(VkCommandBuffer, GpuProfiler&);
    void checkDeclared(uint32_t, ResourceRef) const;
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
        uint32_t use = 0; // index into the pass's own declarations
        bool clear = false;
        VkClearValue value{};
    };
    // A pass owns its declarations. They used to be a slice of one shared array, which
    // made the slice correct only while nothing else was declared in between — declaring
    // one pass while another builder was still open silently handed its resources to the
    // wrong pass. Ownership is what removes that ordering rule.
    struct Pass {
        std::string name;
        std::vector<Use> uses;
        std::vector<Use> resolvedUses;
        std::vector<ShaderAccess> shaders;
        std::vector<ShaderBinding> mappings, bindings;
        std::vector<Attachment> colors;
        std::vector<uint32_t> edges; // producing passes, in declaration order
        ResourceId depth;
        uint32_t depthUse = 0;
        float depthClear = 1.f;
        bool depthLoad = false;
        std::shared_ptr<const ProgramStorage> program;
        std::vector<uint8_t> constants;
        Extent3D elements, localSize;
        ResourceId dispatchExtent;
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
    // Passes outlive reset() so that a frame reuses the storage its declarations needed
    // last time; declared_ is how many of them this frame has.
    std::vector<Pass> passes_;
    uint32_t declared_ = 0;
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
    void resolveShaders();
    void analyse();
    void cull();
    void validate() const;
    void liveness();
    void plan(uint32_t width, uint32_t height);
    void synchronise(VkCommandBuffer, const Use*, uint32_t count);
    void handover(VkCommandBuffer);
};
} // namespace whimsical::rg
