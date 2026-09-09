#pragma once
#include "ProgramStorage.h"
#include <memory>

namespace whimsical::rc {
// Native pipeline description. Callers choose shader interfaces and raster policy;
// the backend owns shader modules, Vulkan translation and pipeline lifetime.
using Pipeline = std::shared_ptr<const rg::ProgramStorage>;
struct ShaderCode {
    const uint32_t* data;
    size_t bytes;
    const char* entry = "main";
    explicit ShaderCode(const std::vector<uint32_t>& code) : data(code.data()), bytes(code.size() * 4) {}
    ShaderCode(const uint32_t* code, size_t size, const char* entryPoint = "main")
        : data(code), bytes(size), entry(entryPoint) {}
};
struct GraphicsDescription {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkFormat> colors;
    std::vector<VkPipelineColorBlendAttachmentState> blend;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depthTest = false, depthWrite = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
};
Pipeline graphicsProgram(VulkanContext&, const GraphicsDescription&, ShaderCode vertex, ShaderCode fragment);
Pipeline computeProgram(VulkanContext&, VkPipelineLayout, ShaderCode);
} // namespace whimsical::rc
