#pragma once
#include "GraphTypes.h"
#include <vector>

namespace afterlight::rg {
// The storage shape two resources must agree on before one can reuse the other's memory,
// and equally the test for whether an existing allocation still fits its declaration.
uint64_t storageSignature(const Declaration&, uint32_t width, uint32_t height);

// What compiling the frame decided about one logical resource: whether its contents are
// needed at all, and whose storage it borrows when they are. A transient no live pass
// touches is not needed, so the pool holds nothing for it — allocation is a consequence of
// the same liveness that drove culling rather than a separate opinion about the frame.
struct Residency {
    ResourceId root;     // itself when the resource owns its storage
    bool needed = false; // graph-owned resources only; the rest are the owner's business
};

// Physical side of the registry: one Vulkan object per live slot, per-pass descriptor
// sets, and the access state the compiler synchronises against. Logical
// resources reach their memory only through here, so a history pair can swap halves and a
// transient can share storage without any shader or pass knowing.
class ResourcePool {
  public:
    ResourcePool(VulkanContext&, const Registry&);
    ~ResourcePool();

    // Owner-allocated resources. The pool keeps a reference rather than a copy, so the
    // owner's image layout and the graph's barriers stay one fact, and an owner that grows
    // or rebuilds its resource in place does not have to say so twice.
    void importImage(ResourceId, Image&);
    void importBuffer(ResourceId, Buffer&);
    void importTlas(ResourceId, VkAccelerationStructureKHR, uint64_t generation);
    void importSamplers(ResourceId, std::vector<VkDescriptorImageInfo>);

    // Makes the pool match what compiling the frame decided: resources the frame does not
    // need hold no memory, the rest either own their storage or borrow the storage of the
    // resource named as their root. Only slots whose storage actually changed are
    // reallocated, so a frame that grows a pass keeps every history and persistent
    // resource it had.
    void realize(uint32_t width, uint32_t height, const std::vector<Residency>&);
    void flip() {
        parity_ ^= 1;
    }

    uint32_t width() const {
        return width_;
    }
    uint32_t height() const {
        return height_;
    }
    uint32_t extentWidth(ResourceId) const;
    uint32_t extentHeight(ResourceId) const;

    uint32_t physical(ResourceRef) const;
    Image& image(ResourceRef ref) {
        auto& slot = slots_[physical(ref)];
        return slot.external ? *slot.external : slot.image;
    }
    Buffer& buffer(ResourceRef ref) {
        auto& slot = slots_[physical(ref)];
        return slot.host ? *slot.host : slot.buffer;
    }
    AccessState& state(uint32_t physicalSlot);
    const Registry& registry() const {
        return registry_;
    }
    const Declaration& declaration(ResourceId id) const {
        return registry_[id];
    }
    size_t resourceCount() const {
        return registry_.size();
    }

    VkDescriptorSetLayout setLayout() const {
        return setLayout_;
    }
    VkPipelineLayout pipelineLayout() const {
        return pipelineLayout_;
    }
    // The pass's set for this history parity, updated only for required shader bindings.
    // The caller waits for the previous frame before updating or destroying its objects.
    VkDescriptorSet descriptors(uint32_t pass, const std::vector<ShaderBinding>&);

    uint64_t ownedBytes() const;    // what the transient reuse actually allocated
    uint64_t declaredBytes() const; // what the same declarations would cost without reuse
    uint64_t descriptorWrites() const {
        return descriptorWrites_;
    }

  private:
    // One physical resource. A history declaration owns two of these and swaps which one
    // the "current" view points at; an aliased transient owns none and borrows another's.
    struct Physical {
        Image image;
        Buffer buffer;
        Image* external = nullptr; // Lifetime::Imported
        Buffer* host = nullptr;    // Lifetime::External
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        std::vector<VkDescriptorImageInfo> samplers;
        uint64_t generation = 0, stateGeneration = 0;
        uint64_t samplerRevision = 0; // a descriptor array has no single handle to compare
        AccessState state;
        uint64_t signature = 0; // storage shape; unchanged means keep the allocation
        uint64_t bytes = 0;
    };
    struct Bound {
        uint64_t object = 0, extent = 0, generation = 0;
        bool operator==(const Bound& other) const {
            return object == other.object && extent == other.extent && generation == other.generation;
        }
    };
    // Each pass has immutable bindings while its commands are in flight. Rewriting one
    // shared set between dispatches would change the bindings of earlier dispatches too.
    struct DescriptorBatch {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[2]{};
        std::vector<Bound> bound;
    };
    std::vector<DescriptorBatch> descriptors_;
    uint64_t generation(const Physical&) const;
    void allocateDescriptors(DescriptorBatch&);
    VulkanContext& vk_;
    const Registry& registry_;
    std::vector<Physical> slots_;
    std::vector<uint16_t> root_; // physical slot -> slot that owns the storage
    std::vector<uint32_t> bases_;
    uint32_t parity_ = 0, width_ = 0, height_ = 0;
    uint64_t declaredBytes_ = 0, descriptorWrites_ = 0, samplerRevisions_ = 0;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    void createLayout();
    void release(Physical&);
    uint32_t base(ResourceId id) const {
        return bases_[id.index];
    }
};
} // namespace afterlight::rg
