#include "ResourcePool.h"
#include <algorithm>
#include <map>

namespace afterlight::rg {
namespace {
VkFormat vulkanFormat(Format format) {
    switch (format) {
    case Format::RGBA16F:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RGBA32F:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::RG16F:
        return VK_FORMAT_R16G16_SFLOAT;
    case Format::R16F:
        return VK_FORMAT_R16_SFLOAT;
    case Format::R32F:
        return VK_FORMAT_R32_SFLOAT;
    case Format::RGBA8:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::D32:
        return VK_FORMAT_D32_SFLOAT;
    }
    throw std::runtime_error("Unknown render graph format");
}
VkDescriptorType descriptorType(BindingType type) {
    switch (type) {
    case BindingType::Uniform:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingType::Storage:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case BindingType::StorageImage:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case BindingType::SamplerArray:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case BindingType::Tlas:
        return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    case BindingType::None:
        break;
    }
    throw std::runtime_error("Unbound render graph view");
}
uint32_t divide(uint32_t value, uint16_t divisor) {
    return (value + divisor - 1) / divisor;
}
} // namespace

uint64_t storageSignature(const Declaration& declaration, uint32_t width, uint32_t height) {
    if (declaration.kind != Kind::Image)
        return (uint64_t(1) << 63) | (declaration.bytes(width, height) << 1) |
               uint64_t(declaration.readback);
    return (uint64_t(divide(width, declaration.divisor)) << 40) |
           (uint64_t(divide(height, declaration.divisor)) << 12) |
           (uint64_t(declaration.format) << 1) | 1;
}

ResourcePool::ResourcePool(VulkanContext& vk, const Registry& registry) : vk_(vk), registry_(registry) {
    bases_.resize(registry_.size());
    for (uint16_t i = 0; i < registry_.size(); ++i)
        bases_[i] = registry_.physicalBase({i});
    slots_.resize(registry_.physicalCount());
    root_.resize(slots_.size());
    aliasRoot_.resize(registry_.size());
    for (uint16_t i = 0; i < root_.size(); ++i)
        root_[i] = i;
    for (uint16_t i = 0; i < registry_.size(); ++i)
        aliasRoot_[i] = ResourceId{i};
    createLayout();
}

ResourcePool::~ResourcePool() {
    for (auto& slot : slots_)
        release(slot);
    if (pipelineLayout_)
        vkDestroyPipelineLayout(vk_.device, pipelineLayout_, nullptr);
    if (descriptorPool_)
        vkDestroyDescriptorPool(vk_.device, descriptorPool_, nullptr);
    if (setLayout_)
        vkDestroyDescriptorSetLayout(vk_.device, setLayout_, nullptr);
}

// One descriptor set per history parity. Both are written together; flipping picks the
// one whose "previous" bindings point at the half the last frame wrote.
void ResourcePool::createLayout() {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::map<VkDescriptorType, uint32_t> counts;
    auto add = [&](const Declaration& declaration, const ShaderView& view) {
        if (!view)
            return;
        auto type = descriptorType(view.type);
        auto stages = declaration.section == Section::Shared
                          ? VkShaderStageFlags(VK_SHADER_STAGE_ALL)
                          : VkShaderStageFlags(VK_SHADER_STAGE_COMPUTE_BIT);
        bindings.push_back({view.binding, type, view.count, stages, nullptr});
        counts[type] += view.count * 2;
    };
    for (uint16_t i = 0; i < registry_.size(); ++i) {
        const auto& declaration = registry_[{i}];
        add(declaration, declaration.view);
        add(declaration, declaration.previous);
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = uint32_t(bindings.size());
    sl.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(vk_.device, &sl, nullptr, &setLayout_));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(vk_.device, &pl, nullptr, &pipelineLayout_));
    std::vector<VkDescriptorPoolSize> sizes;
    for (auto [type, count] : counts)
        sizes.push_back({type, count});
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 2;
    dp.poolSizeCount = uint32_t(sizes.size());
    dp.pPoolSizes = sizes.data();
    VK_CHECK(vkCreateDescriptorPool(vk_.device, &dp, nullptr, &descriptorPool_));
    VkDescriptorSetLayout layouts[2]{setLayout_, setLayout_};
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = descriptorPool_;
    da.descriptorSetCount = 2;
    da.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk_.device, &da, sets_));
}

void ResourcePool::release(Physical& slot) {
    if (!slot.signature)
        return;
    vk_.destroy(slot.image);
    vk_.destroy(slot.buffer);
    slot.state = {};
    slot.signature = 0;
    slot.bytes = 0;
    descriptorsDirty_ = true;
}

void ResourcePool::importImage(ResourceId id, Image& image) {
    auto& slot = slots_[base(id)];
    descriptorsDirty_ = descriptorsDirty_ || !slot.external || slot.external->view != image.view;
    slot.external = &image;
}

void ResourcePool::importBuffer(ResourceId id, Buffer& buffer) {
    auto& slot = slots_[base(id)];
    descriptorsDirty_ = descriptorsDirty_ || !slot.host || slot.host->handle != buffer.handle ||
                        slot.host->size != buffer.size;
    slot.host = &buffer;
}

void ResourcePool::importTlas(ResourceId id, VkAccelerationStructureKHR handle) {
    auto& slot = slots_[base(id)];
    descriptorsDirty_ = descriptorsDirty_ || slot.tlas != handle;
    slot.tlas = handle;
}

void ResourcePool::importSamplers(ResourceId id, std::vector<VkDescriptorImageInfo> samplers) {
    slots_[base(id)].samplers = std::move(samplers);
    descriptorsDirty_ = true;
}

uint32_t ResourcePool::extentWidth(ResourceId id) const {
    return divide(width_, registry_[id].divisor);
}
uint32_t ResourcePool::extentHeight(ResourceId id) const {
    return divide(height_, registry_[id].divisor);
}

uint32_t ResourcePool::physical(ResourceRef ref) const {
    const auto& declaration = registry_[ref.id];
    uint32_t index = bases_[ref.id.index];
    if (declaration.lifetime == Lifetime::History)
        index += ref.slot == Slot::Previous ? (parity_ ^ 1) : parity_;
    return root_[index];
}

void ResourcePool::realize(uint32_t width, uint32_t height, const std::vector<ResourceId>& aliasRoot) {
    width_ = width;
    height_ = height;
    aliasRoot_ = aliasRoot;
    declaredBytes_ = 0;
    for (uint16_t i = 0; i < registry_.size(); ++i) {
        const auto& declaration = registry_[{i}];
        if (!graphOwned(declaration.lifetime))
            continue;
        const uint32_t w = divide(width, declaration.divisor), h = divide(height, declaration.divisor);
        const uint64_t bytes = declaration.kind == Kind::Image
                                   ? uint64_t(w) * h * formatInfo(declaration.format).bytes
                                   : declaration.bytes(width, height);
        const uint32_t halves = declaration.lifetime == Lifetime::History ? 2 : 1;
        declaredBytes_ += bytes * halves;
        if (aliasRoot_[i].index != i) {
            release(slots_[bases_[i]]);
            root_[bases_[i]] = uint16_t(bases_[aliasRoot_[i].index]);
            continue;
        }
        const uint64_t signature = storageSignature(declaration, width, height);
        for (uint32_t half = 0; half < halves; ++half) {
            const uint32_t index = bases_[i] + half;
            root_[index] = uint16_t(index);
            auto& slot = slots_[index];
            if (slot.signature == signature)
                continue;
            release(slot);
            if (declaration.kind == Kind::Image) {
                // Every colour image gets every colour capability. Storage already rules
                // out framebuffer compression, so the extra usage costs nothing and keeps
                // one storage class per format instead of splitting it by role.
                VkImageUsageFlags usage =
                    declaration.format == Format::D32
                        ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        : VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                slot.image = vk_.image(w, h, vulkanFormat(declaration.format), usage);
            } else if (declaration.readback)
                slot.buffer = vk_.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
            else
                slot.buffer = vk_.buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            slot.signature = signature;
            slot.bytes = bytes;
            descriptorsDirty_ = true;
        }
    }
}

VkDescriptorSet ResourcePool::descriptors() {
    if (!descriptorsDirty_)
        return sets_[parity_];
    descriptorsDirty_ = false;
    const size_t capacity = size_t(registry_.bindingCount()) * 2;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> structures;
    std::vector<VkWriteDescriptorSet> writes;
    buffers.reserve(capacity);
    images.reserve(capacity);
    structures.reserve(capacity);
    writes.reserve(capacity);
    for (uint32_t parity = 0; parity < 2; ++parity)
        for (uint16_t i = 0; i < registry_.size(); ++i) {
            const auto& declaration = registry_[{i}];
            for (auto half : {Slot::Current, Slot::Previous}) {
                const auto& view = half == Slot::Previous ? declaration.previous : declaration.view;
                if (!view)
                    continue;
                uint32_t index = bases_[i];
                if (declaration.lifetime == Lifetime::History)
                    index += half == Slot::Previous ? (parity ^ 1) : parity;
                auto& slot = slots_[root_[index]];
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = sets_[parity];
                write.dstBinding = view.binding;
                write.descriptorCount = 1;
                write.descriptorType = descriptorType(view.type);
                switch (view.type) {
                case BindingType::Uniform:
                case BindingType::Storage: {
                    const auto& buffer = slot.host ? *slot.host : slot.buffer;
                    buffers.push_back({buffer.handle, 0, buffer.size});
                    write.pBufferInfo = &buffers.back();
                    break;
                }
                case BindingType::StorageImage:
                    images.push_back({VK_NULL_HANDLE,
                                      slot.external ? slot.external->view : slot.image.view,
                                      VK_IMAGE_LAYOUT_GENERAL});
                    write.pImageInfo = &images.back();
                    break;
                case BindingType::SamplerArray:
                    write.descriptorCount = view.count;
                    write.pImageInfo = slot.samplers.data();
                    break;
                case BindingType::Tlas:
                    structures.push_back(
                        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR});
                    structures.back().accelerationStructureCount = 1;
                    structures.back().pAccelerationStructures = &slot.tlas;
                    write.pNext = &structures.back();
                    break;
                case BindingType::None:
                    break;
                }
                writes.push_back(write);
            }
        }
    vkUpdateDescriptorSets(vk_.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    return sets_[parity_];
}

uint64_t ResourcePool::ownedBytes() const {
    uint64_t total = 0;
    for (const auto& slot : slots_)
        total += slot.bytes;
    return total;
}

uint64_t ResourcePool::declaredBytes() const {
    return declaredBytes_;
}
} // namespace afterlight::rg
