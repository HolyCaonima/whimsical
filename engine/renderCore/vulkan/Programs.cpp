#include "Programs.h"

namespace whimsical::rc {
namespace {
struct Module {
    VulkanContext& vk;
    VkShaderModule handle = VK_NULL_HANDLE;
    Module(VulkanContext& device, ShaderCode code) : vk(device) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.bytes;
        info.pCode = code.data;
        VK_CHECK(vkCreateShaderModule(vk.device, &info, nullptr, &handle));
    }
    ~Module() { vkDestroyShaderModule(vk.device, handle, nullptr); }
};
VkPipelineShaderStageCreateInfo stage(VkShaderStageFlagBits type, const Module& module, const char* entry) {
    VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage = type;
    info.module = module.handle;
    info.pName = entry;
    return info;
}
}
Pipeline computeProgram(VulkanContext& vk, VkPipelineLayout layout, ShaderCode code) {
    Module module(vk, code);
    auto storage = std::make_shared<rg::ProgramStorage>(vk);
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.layout = layout;
    info.stage = stage(VK_SHADER_STAGE_COMPUTE_BIT, module, code.entry);
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &storage->pipeline));
    return storage;
}
Pipeline graphicsProgram(VulkanContext& vk, const GraphicsDescription& desc, ShaderCode vertex, ShaderCode fragment) {
    if (desc.colors.size() != desc.blend.size())
        throw std::invalid_argument("Each color attachment requires a blend description");
    Module vs(vk, vertex), fs(vk, fragment);
    VkPipelineShaderStageCreateInfo stages[] = {stage(VK_SHADER_STAGE_VERTEX_BIT, vs, vertex.entry),
                                               stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs, fragment.entry)};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(desc.bindings.size());
    vi.pVertexBindingDescriptions = desc.bindings.data();
    vi.vertexAttributeDescriptionCount = uint32_t(desc.attributes.size());
    vi.pVertexAttributeDescriptions = desc.attributes.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = desc.topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cull;
    raster.frontFace = desc.frontFace;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = desc.depthTest;
    depth.depthWriteEnable = desc.depthWrite;
    depth.depthCompareOp = desc.depthCompare;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = uint32_t(desc.blend.size());
    blend.pAttachments = desc.blend.data();
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = uint32_t(desc.colors.size());
    rendering.pColorAttachmentFormats = desc.colors.data();
    rendering.depthAttachmentFormat = desc.depth;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = desc.layout;
    auto storage = std::make_shared<rg::ProgramStorage>(vk);
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &storage->pipeline));
    return storage;
}
} // namespace whimsical::rc
