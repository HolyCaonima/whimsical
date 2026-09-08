#include "NrdDenoiser.h"
#include "core/CpuProfile.h"
#include "GpuProfiler.h"
#include <cstring>
#include <algorithm>
#include <iostream>
namespace afterlight {
static void nrdCheck(nrd::Result r) {
    if (r != nrd::Result::SUCCESS)
        throw std::runtime_error("NRD API failed: " + std::to_string(uint32_t(r)));
}
static VkFormat format(nrd::Format f) {
    // Ordered exactly like NRD 4.17 Format; all pool formats are represented.
    static const VkFormat formats[] = {VK_FORMAT_R8_UNORM,
                                       VK_FORMAT_R8_SNORM,
                                       VK_FORMAT_R8_UINT,
                                       VK_FORMAT_R8_SINT,
                                       VK_FORMAT_R8G8_UNORM,
                                       VK_FORMAT_R8G8_SNORM,
                                       VK_FORMAT_R8G8_UINT,
                                       VK_FORMAT_R8G8_SINT,
                                       VK_FORMAT_R8G8B8A8_UNORM,
                                       VK_FORMAT_R8G8B8A8_SNORM,
                                       VK_FORMAT_R8G8B8A8_UINT,
                                       VK_FORMAT_R8G8B8A8_SINT,
                                       VK_FORMAT_R8G8B8A8_SRGB,
                                       VK_FORMAT_R16_UNORM,
                                       VK_FORMAT_R16_SNORM,
                                       VK_FORMAT_R16_UINT,
                                       VK_FORMAT_R16_SINT,
                                       VK_FORMAT_R16_SFLOAT,
                                       VK_FORMAT_R16G16_UNORM,
                                       VK_FORMAT_R16G16_SNORM,
                                       VK_FORMAT_R16G16_UINT,
                                       VK_FORMAT_R16G16_SINT,
                                       VK_FORMAT_R16G16_SFLOAT,
                                       VK_FORMAT_R16G16B16A16_UNORM,
                                       VK_FORMAT_R16G16B16A16_SNORM,
                                       VK_FORMAT_R16G16B16A16_UINT,
                                       VK_FORMAT_R16G16B16A16_SINT,
                                       VK_FORMAT_R16G16B16A16_SFLOAT,
                                       VK_FORMAT_R32_UINT,
                                       VK_FORMAT_R32_SINT,
                                       VK_FORMAT_R32_SFLOAT,
                                       VK_FORMAT_R32G32_UINT,
                                       VK_FORMAT_R32G32_SINT,
                                       VK_FORMAT_R32G32_SFLOAT,
                                       VK_FORMAT_R32G32B32_UINT,
                                       VK_FORMAT_R32G32B32_SINT,
                                       VK_FORMAT_R32G32B32_SFLOAT,
                                       VK_FORMAT_R32G32B32A32_UINT,
                                       VK_FORMAT_R32G32B32A32_SINT,
                                       VK_FORMAT_R32G32B32A32_SFLOAT,
                                       VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                       VK_FORMAT_A2B10G10R10_UINT_PACK32,
                                       VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                       VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    static_assert(sizeof(formats) / sizeof(formats[0]) == size_t(nrd::Format::MAX_NUM),
                  "NRD format map is out of date");
    return formats[uint32_t(f)];
}
NrdDenoiser::NrdDenoiser(VulkanContext& v) : vk_(v) {
    nrd::DenoiserDesc denoiser{0, nrd::Denoiser::RELAX_DIFFUSE_SPECULAR};
    nrd::InstanceCreationDesc creation{};
    creation.denoisers = &denoiser;
    creation.denoisersNum = 1;
    nrdCheck(nrd::CreateInstance(creation, instance_));
    desc_ = nrd::GetInstanceDesc(*instance_);
    const auto& offsets = nrd::GetLibraryDesc()->spirvBindingOffsets;
    if (desc_->resourcesSpaceIndex != 0 || desc_->constantBufferAndSamplersSpaceIndex != 1)
        throw std::runtime_error("Unexpected NRD descriptor spaces");
    std::vector<VkDescriptorSetLayoutBinding> constantsBindings;
    for (uint32_t i = 0; i < 2; i++) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0;
        VK_CHECK(vkCreateSampler(vk_.device, &si, nullptr, &samplers_[i]));
        constantsBindings.push_back({offsets.samplerOffset + desc_->samplersBaseRegisterIndex + i,
                                     VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                     &samplers_[i]});
    }
    constantsBindings.push_back({offsets.constantBufferOffset + desc_->constantBufferRegisterIndex,
                                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = uint32_t(constantsBindings.size());
    sl.pBindings = constantsBindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(vk_.device, &sl, nullptr, &constantsLayout_));
    for (uint32_t i = 0; i < desc_->pipelinesNum; i++) {
        const auto& p = desc_->pipelines[i];
        Pipeline out;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        uint32_t reads = 0, writes = 0;
        for (uint32_t range = 0; range < p.resourceRangesNum; range++) {
            auto& rr = p.resourceRanges[range];
            for (uint32_t k = 0; k < rr.descriptorsNum; k++)
                if (rr.descriptorType == nrd::DescriptorType::TEXTURE)
                    bindings.push_back({offsets.textureOffset + desc_->resourcesBaseRegisterIndex + reads++,
                                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                        nullptr});
                else
                    bindings.push_back(
                        {offsets.storageTextureAndBufferOffset + desc_->resourcesBaseRegisterIndex + writes++,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        sl.bindingCount = uint32_t(bindings.size());
        sl.pBindings = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(vk_.device, &sl, nullptr, &out.resources));
        VkDescriptorSetLayout layouts[]{out.resources, constantsLayout_};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2;
        pl.pSetLayouts = layouts;
        VK_CHECK(vkCreatePipelineLayout(vk_.device, &pl, nullptr, &out.layout));
        if (!p.computeShaderSPIRV.bytecode || !p.computeShaderSPIRV.size)
            throw std::runtime_error("NRD SPIR-V was not embedded");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = size_t(p.computeShaderSPIRV.size);
        sm.pCode = static_cast<const uint32_t*>(p.computeShaderSPIRV.bytecode);
        VkShaderModule module;
        VK_CHECK(vkCreateShaderModule(vk_.device, &sm, nullptr, &module));
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.layout = out.layout;
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = module;
        cp.stage.pName = desc_->shaderEntryPoint;
        auto result = vkCreateComputePipelines(vk_.device, VK_NULL_HANDLE, 1, &cp, nullptr, &out.handle);
        vkDestroyShaderModule(vk_.device, module, nullptr);
        VK_CHECK(result);
        pipelines_.push_back(out);
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};
    VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pc.maxSets = 1024;
    pc.poolSizeCount = 4;
    pc.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk_.device, &pc, nullptr, &pool_));
    constants_ = vk_.buffer(1024 * 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, BufferMemory::Upload);
    nrd::RelaxSettings settings{};
    settings.diffuseMaxAccumulatedFrameNum = 24;
    settings.specularMaxAccumulatedFrameNum = 24;
    settings.atrousIterationNum = 5;
    // Both lobes are sampled every pixel. ReSTIR already performs spatial reuse;
    // diffuse pre-blur loses shadow edges before the luminance-aware A-trous pass.
    // Start from RTXGI's RELAX edge settings, keeping one combined DI/GI denoiser.
    settings.diffusePrepassBlurRadius = 0;
    settings.specularPrepassBlurRadius = 20;
    settings.diffusePhiLuminance = 0.5f;
    settings.specularPhiLuminance = 0.35f;
    settings.diffuseMaxFastAccumulatedFrameNum = 4;
    settings.enableAntiFirefly = true;
    nrdCheck(nrd::SetDenoiserSettings(*instance_, 0, &settings));
    std::cout << "NRD RELAX_DIFFUSE_SPECULAR: " << pipelines_.size() << " native Vulkan pipelines\n";
}
NrdDenoiser::~NrdDenoiser() {
    for (auto& i : permanent_)
        vk_.destroy(i);
    for (auto& i : transient_)
        vk_.destroy(i);
    vk_.destroy(constants_);
    if (pool_)
        vkDestroyDescriptorPool(vk_.device, pool_, nullptr);
    for (auto& p : pipelines_) {
        vkDestroyPipeline(vk_.device, p.handle, nullptr);
        vkDestroyPipelineLayout(vk_.device, p.layout, nullptr);
        vkDestroyDescriptorSetLayout(vk_.device, p.resources, nullptr);
    }
    if (constantsLayout_)
        vkDestroyDescriptorSetLayout(vk_.device, constantsLayout_, nullptr);
    for (auto s : samplers_)
        if (s)
            vkDestroySampler(vk_.device, s, nullptr);
    if (instance_)
        nrd::DestroyInstance(*instance_);
}
void NrdDenoiser::resize(uint32_t w, uint32_t h) {
    width_ = w;
    height_ = h;
    first_ = true;
    for (auto& i : permanent_)
        vk_.destroy(i);
    for (auto& i : transient_)
        vk_.destroy(i);
    permanent_.clear();
    transient_.clear();
    auto allocate = [&](const nrd::TextureDesc* textures, uint32_t count, std::vector<Image>& images) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t d = textures[i].downsampleFactor;
            images.push_back(vk_.image((w + d - 1) / d, (h + d - 1) / d, format(textures[i].format),
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        }
    };
    allocate(desc_->permanentPool, desc_->permanentPoolSize, permanent_);
    allocate(desc_->transientPool, desc_->transientPoolSize, transient_);
}
void NrdDenoiser::dispatch(VkCommandBuffer command,
                           const std::array<Image*, size_t(nrd::ResourceType::MAX_NUM)>& resources,
                           const Camera& camera, uint32_t frame, bool reset, float ms,
                           GpuProfiler& profiler) {
    VK_CHECK(vkResetDescriptorPool(vk_.device, pool_, 0));
    // NRD reconstructs texture UV with D3D convention; remove Vulkan's projection-Y flip.
    mat4 projection = camera.projection(float(width_) / height_);
    projection[1][1] *= -1;
    mat4 view = camera.view();
    if (first_) {
        previousView_ = view;
        previousProjection_ = projection;
    }
    nrd::CommonSettings common;
    std::memcpy(common.viewToClipMatrix, glm::value_ptr(projection), 64);
    std::memcpy(common.viewToClipMatrixPrev, glm::value_ptr(previousProjection_), 64);
    std::memcpy(common.worldToViewMatrix, glm::value_ptr(view), 64);
    std::memcpy(common.worldToViewMatrixPrev, glm::value_ptr(previousView_), 64);
    common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] =
        uint16_t(width_);
    common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] =
        uint16_t(height_);
    common.frameIndex = frame;
    common.isHistoryConfidenceAvailable = true;
    common.timeDeltaBetweenFrames = std::max(ms, 1.f);
    common.denoisingRange = 150;
    common.accumulationMode =
        first_ || reset ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
    nrdCheck(nrd::SetCommonSettings(*instance_, common));
    for (auto& collection : {&permanent_, &transient_})
        for (auto& i : *collection) {
            if (i.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                vk_.transition(command, i, VK_IMAGE_LAYOUT_GENERAL);
                VkClearColorValue zero{};
                VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(command, i.handle, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
            }
        }
    VulkanContext::barrier(command);
    nrd::Identifier id = 0;
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t count = 0;
    nrdCheck(nrd::GetComputeDispatches(*instance_, &id, 1, dispatches, count));
    VkDeviceSize cursor = 0,
                 alignment =
                     std::max<VkDeviceSize>(256, vk_.properties.limits.minUniformBufferOffsetAlignment);
    auto offsets = nrd::GetLibraryDesc()->spirvBindingOffsets;
    for (uint32_t d = 0; d < count; d++) {
        const auto& job = dispatches[d];
        CpuScope cpuScope(job.name);
        GpuScope scope(profiler, command, job.name);
        auto& pipeline = pipelines_.at(job.pipelineIndex);
        VkDescriptorSetLayout layouts[]{pipeline.resources, constantsLayout_};
        VkDescriptorSet sets[2];
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        VK_CHECK(vkAllocateDescriptorSets(vk_.device, &ai, sets));
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorImageInfo> images(job.resourcesNum);
        uint32_t readIndex = 0, writeIndex = 0;
        for (uint32_t r = 0; r < job.resourcesNum; r++) {
            auto& request = job.resources[r];
            Image* image =
                request.type == nrd::ResourceType::PERMANENT_POOL   ? &permanent_.at(request.indexInPool)
                : request.type == nrd::ResourceType::TRANSIENT_POOL ? &transient_.at(request.indexInPool)
                                                                    : resources[size_t(request.type)];
            if (!image)
                throw std::runtime_error(std::string("Unbound NRD resource: ") +
                                         nrd::GetResourceTypeString(request.type));
            bool storage = request.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            // Resources the render graph handed us already arrive in GENERAL and visible
            // to compute; only NRD's own pool needs a transition, and only once.
            if (image->layout != VK_IMAGE_LAYOUT_GENERAL)
                vk_.transition(command, *image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               storage ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                       : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            images[r] = {VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = sets[0];
            write.dstBinding = desc_->resourcesBaseRegisterIndex +
                               (storage ? offsets.storageTextureAndBufferOffset + writeIndex++
                                        : offsets.textureOffset + readIndex++);
            write.descriptorCount = 1;
            write.descriptorType =
                storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &images[r];
            writes.push_back(write);
        }
        auto bytes = std::max(16u, job.constantBufferDataSize);
        if (cursor + bytes > constants_.size)
            throw std::runtime_error("NRD constants overflow");
        if (job.constantBufferDataSize)
            std::memcpy(static_cast<uint8_t*>(constants_.mapped) + cursor, job.constantBufferData,
                        job.constantBufferDataSize);
        VkDescriptorBufferInfo cb{constants_.handle, cursor, bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[1];
        write.dstBinding = offsets.constantBufferOffset + desc_->constantBufferRegisterIndex;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &cb;
        writes.push_back(write);
        cursor = (cursor + bytes + alignment - 1) / alignment * alignment;
        vkUpdateDescriptorSets(vk_.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 2, sets, 0,
                                nullptr);
        vkCmdDispatch(command, job.gridWidth, job.gridHeight, 1);
        VulkanContext::barrier(command);
    }
    previousView_ = view;
    previousProjection_ = projection;
    first_ = false;
}
} // namespace afterlight
