#pragma once
#include "GraphTypes.h"
#include <vector>

namespace afterlight::rg {
// The storage shape two resources must agree on before one can reuse the other's memory,
// and equally the test for whether an existing allocation still fits its declaration.
uint64_t storageSignature(const Declaration&, uint32_t width, uint32_t height);

// Physical side of the registry: one Vulkan object per live slot, the descriptor set the
// whole pipeline binds, and the access state the compiler synchronises against. Logical
// resources reach their memory only through here, so a history pair can swap halves and a
// transient can share storage without any shader or pass knowing.
class ResourcePool {
  public:
    ResourcePool(VulkanContext&, const Registry&);
    ~ResourcePool();

    // Owner-allocated resources. The pool keeps a reference rather than a copy, so the
    // owner's image layout and the graph's barriers stay one fact.
    void importImage(ResourceId, Image&);
    void importBuffer(ResourceId, Buffer&);
    void importTlas(ResourceId, VkAccelerationStructureKHR);
    void importSamplers(ResourceId, std::vector<VkDescriptorImageInfo>);

    // aliasRoot maps each logical resource to the resource whose storage it shares; a
    // resource that owns its own storage maps to itself. The compiler produces it from
    // transient lifetimes, so reuse is a consequence of the graph rather than a decision.
    // Only slots whose storage actually changed are reallocated, so a frame that grows a
    // pass keeps every history and persistent resource it had.
    void realize(uint32_t width, uint32_t height, const std::vector<ResourceId>& aliasRoot);
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
    AccessState& state(uint32_t physicalSlot) {
        return slots_[physicalSlot].state;
    }
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
    VkDescriptorSet descriptors();

    uint64_t ownedBytes() const;    // what the transient reuse actually allocated
    uint64_t declaredBytes() const; // what the same declarations would cost without reuse

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
        AccessState state;
        uint64_t signature = 0; // storage shape; unchanged means keep the allocation
        uint64_t bytes = 0;
    };
    VulkanContext& vk_;
    const Registry& registry_;
    std::vector<Physical> slots_;
    std::vector<uint16_t> root_; // physical slot -> slot that owns the storage
    std::vector<uint32_t> bases_;
    std::vector<ResourceId> aliasRoot_;
    uint32_t parity_ = 0, width_ = 0, height_ = 0;
    uint64_t declaredBytes_ = 0;
    bool descriptorsDirty_ = true;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[2]{};

    void createLayout();
    void release(Physical&);
    uint32_t base(ResourceId id) const {
        return bases_[id.index];
    }
};
} // namespace afterlight::rg
