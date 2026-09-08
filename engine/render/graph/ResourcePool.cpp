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
               uint64_t(hostRead(declaration));
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
    for (uint16_t i = 0; i < root_.size(); ++i)
        root_[i] = i;
    createLayout();
}

ResourcePool::~ResourcePool() {
    for (auto& slot : slots_)
        release(slot);
    if (pipelineLayout_)
        vkDestroyPipelineLayout(vk_.device, pipelineLayout_, nullptr);
    for (auto& batch : descriptors_)
        if (batch.pool)
            vkDestroyDescriptorPool(vk_.device, batch.pool, nullptr);
    if (setLayout_)
        vkDestroyDescriptorSetLayout(vk_.device, setLayout_, nullptr);
}

// All pass sets share the registry layout. Allocation of sets follows actual pass use.
void ResourcePool::createLayout() {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    auto add = [&](const Declaration& declaration, const ShaderView& view) {
        if (!view)
            return;
        auto type = descriptorType(view.type);
        auto stages = declaration.section == Section::Shared
                          ? VkShaderStageFlags(VK_SHADER_STAGE_ALL)
                          : VkShaderStageFlags(VK_SHADER_STAGE_COMPUTE_BIT);
        bindings.push_back({view.binding, type, view.count, stages, nullptr});
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
}

void ResourcePool::allocateDescriptors(DescriptorBatch& batch) {
    std::map<VkDescriptorType, uint32_t> counts;
    for (uint32_t binding = 0; binding < registry_.bindingCount(); ++binding) {
        const auto& view = registry_.view(registry_.binding(binding));
        counts[descriptorType(view.type)] += view.count * 2;
    }
    batch.bound.resize(size_t(registry_.bindingCount()) * 2);
    std::vector<VkDescriptorPoolSize> sizes;
    for (auto [type, count] : counts)
        sizes.push_back({type, count});
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 2;
    dp.poolSizeCount = uint32_t(sizes.size());
    dp.pPoolSizes = sizes.data();
    VK_CHECK(vkCreateDescriptorPool(vk_.device, &dp, nullptr, &batch.pool));
    VkDescriptorSetLayout layouts[2]{setLayout_, setLayout_};
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = batch.pool;
    da.descriptorSetCount = 2;
    da.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk_.device, &da, batch.sets));
}

void ResourcePool::release(Physical& slot) {
    if (!slot.signature)
        return;
    vk_.destroy(slot.image);
    vk_.destroy(slot.buffer);
    slot.state = {};
    slot.signature = 0;
    slot.bytes = 0;
}

void ResourcePool::importImage(ResourceId id, Image& image) {
    slots_[base(id)].external = &image;
}

void ResourcePool::importBuffer(ResourceId id, Buffer& buffer) {
    slots_[base(id)].host = &buffer;
}

void ResourcePool::importTlas(ResourceId id, VkAccelerationStructureKHR handle, uint64_t generation) {
    slots_[base(id)].tlas = handle;
    slots_[base(id)].generation = generation;
}

// A descriptor array is the one binding with no single object to compare, so its identity
// is the update that produced it.
void ResourcePool::importSamplers(ResourceId id, std::vector<VkDescriptorImageInfo> samplers) {
    auto& slot = slots_[base(id)];
    slot.samplers = std::move(samplers);
    slot.samplerRevision = ++samplerRevisions_;
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

void ResourcePool::realize(uint32_t width, uint32_t height, const std::vector<Residency>& residency) {
    width_ = width;
    height_ = height;
    declaredBytes_ = 0;
    for (uint16_t i = 0; i < registry_.size(); ++i) {
        const auto& declaration = registry_[{i}];
        if (!graphOwned(declaration.lifetime))
            continue;
        const uint32_t halves = declaration.lifetime == Lifetime::History ? 2 : 1;
        if (!residency[i].needed) {
            for (uint32_t half = 0; half < halves; ++half) {
                release(slots_[bases_[i] + half]);
                root_[bases_[i] + half] = uint16_t(bases_[i] + half);
            }
            continue;
        }
        const uint32_t w = divide(width, declaration.divisor), h = divide(height, declaration.divisor);
        const uint64_t bytes = declaration.kind == Kind::Image
                                   ? uint64_t(w) * h * formatInfo(declaration.format).bytes
                                   : declaration.bytes(width, height);
        declaredBytes_ += bytes * halves;
        if (residency[i].root.index != i) {
            release(slots_[bases_[i]]);
            root_[bases_[i]] = uint16_t(bases_[residency[i].root.index]);
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
            } else if (hostRead(declaration))
                slot.buffer = vk_.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
            else
                slot.buffer = vk_.buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            slot.signature = signature;
            slot.bytes = bytes;
        }
    }
}

uint64_t ResourcePool::generation(const Physical& slot) const {
    if (slot.external)
        return slot.external->generation;
    if (slot.host)
        return slot.host->generation;
    if (slot.image.handle)
        return slot.image.generation;
    if (slot.buffer.handle)
        return slot.buffer.generation;
    return slot.generation;
}

AccessState& ResourcePool::state(uint32_t index) {
    auto& slot = slots_[index];
    const auto current = generation(slot);
    if (current != slot.stateGeneration) {
        slot.state = {};
        slot.stateGeneration = current;
    }
    return slot.state;
}

VkDescriptorSet ResourcePool::descriptors(uint32_t pass, const std::vector<ShaderBinding>& bindings) {
    if (descriptors_.size() <= pass)
        descriptors_.resize(size_t(pass) + 1);
    auto& batch = descriptors_[pass];
    if (!batch.pool)
        allocateDescriptors(batch);
    const size_t capacity = bindings.size();
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> structures;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<std::pair<Bound*, Bound>> updates;
    buffers.reserve(capacity);
    images.reserve(capacity);
    structures.reserve(capacity);
    writes.reserve(capacity);
    updates.reserve(capacity);
    for (const auto& binding : bindings) {
        const auto& view = registry_.view(registry_.binding(binding.binding));
        auto& slot = slots_[physical(binding.resource)];
        const auto& buffer = slot.host ? *slot.host : slot.buffer;
        const VkImageView image = slot.external ? slot.external->view : slot.image.view;
        Bound target;
        target.generation = generation(slot);
        switch (view.type) {
        case BindingType::Uniform:
        case BindingType::Storage:
            target.object = uint64_t(buffer.handle);
            target.extent = buffer.size;
            break;
        case BindingType::StorageImage:
            target.object = uint64_t(image);
            break;
        case BindingType::SamplerArray:
            target.object = slot.samplerRevision;
            target.extent = view.count;
            if (slot.samplers.size() != view.count || std::any_of(slot.samplers.begin(), slot.samplers.end(),
                                                                  [](const VkDescriptorImageInfo& image) {
                                                                      return !image.imageView ||
                                                                             !image.sampler;
                                                                  }))
                throw std::runtime_error("Incomplete shader sampler array");
            break;
        case BindingType::Tlas:
            target.object = uint64_t(slot.tlas);
            break;
        case BindingType::None:
            break;
        }
        auto& last = batch.bound[size_t(parity_) * registry_.bindingCount() + binding.binding];
        if (!target.object) {
            last = {};
            throw std::runtime_error("Missing required shader binding: " +
                                     registry_[binding.resource.id].name);
        }
        if (target == last)
            continue;
        updates.push_back({&last, target});
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = batch.sets[parity_];
        write.dstBinding = binding.binding;
        write.descriptorCount = 1;
        write.descriptorType = descriptorType(view.type);
        switch (view.type) {
        case BindingType::Uniform:
        case BindingType::Storage:
            buffers.push_back({buffer.handle, 0, buffer.size});
            write.pBufferInfo = &buffers.back();
            break;
        case BindingType::StorageImage:
            images.push_back({VK_NULL_HANDLE, image, VK_IMAGE_LAYOUT_GENERAL});
            write.pImageInfo = &images.back();
            break;
        case BindingType::SamplerArray:
            write.descriptorCount = view.count;
            write.pImageInfo = slot.samplers.data();
            break;
        case BindingType::Tlas:
            structures.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR});
            structures.back().accelerationStructureCount = 1;
            structures.back().pAccelerationStructures = &slot.tlas;
            write.pNext = &structures.back();
            break;
        case BindingType::None:
            break;
        }
        writes.push_back(write);
    }
    if (!writes.empty())
        vkUpdateDescriptorSets(vk_.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    for (const auto& update : updates)
        *update.first = update.second;
    descriptorWrites_ += writes.size();
    return batch.sets[parity_];
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
