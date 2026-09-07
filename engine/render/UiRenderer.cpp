#include "UiRenderer.h"
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace afterlight {
struct UiRenderer::Impl {
    VulkanContext& vk;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    struct Mesh {
        std::weak_ptr<const ui::Geometry> source;
        Buffer vertices, indices;
    };
    struct Texture {
        std::weak_ptr<const ui::Texture> source;
        Image image;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    std::unordered_map<uint64_t, Mesh> meshes;
    std::unordered_map<uint64_t, Texture> textures;
    Texture white;
    struct Push {
        std::array<float, 16> transform;
        float x, y, width, height;
    };
    VkShaderModule shader(const char* name) {
        auto path = std::string(AFTERLIGHT_SHADERS) + "/" + name + ".spv";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("Missing UI shader: " + path);
        auto size = size_t(file.tellg());
        std::vector<uint32_t> bytes((size + 3) / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = size;
        info.pCode = bytes.data();
        VkShaderModule result;
        VK_CHECK(vkCreateShaderModule(vk.device, &info, nullptr, &result));
        return result;
    }
    explicit Impl(VulkanContext& context) : vk(context) {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 1;
        sl.pBindings = &binding;
        VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &sl, nullptr, &setLayout));
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        VK_CHECK(vkCreatePipelineLayout(vk.device, &pl, nullptr, &layout));
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, nullptr, &sampler));
        auto vertex = shader("ui.vert"), fragment = shader("ui.frag");
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (int i = 0; i < 2; ++i) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = i ? fragment : vertex;
            stages[i].pName = "main";
        }
        VkVertexInputBindingDescription vb{0, sizeof(ui::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                     {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8},
                                                     {2, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable = VK_TRUE;
        attachment.colorWriteMask = 15;
        attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = states;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &rendering;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &viewport;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &blend;
        gp.pDynamicState = &dynamic;
        gp.layout = layout;
        auto result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline);
        vkDestroyShaderModule(vk.device, vertex, nullptr);
        vkDestroyShaderModule(vk.device, fragment, nullptr);
        VK_CHECK(result);
        const uint8_t pixel[] = {255, 255, 255, 255};
        upload(white, 1, 1, pixel);
    }
    void upload(Texture& texture, int width, int height, const uint8_t* pixels) {
        texture.image = vk.image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        auto buffer = vk.buffer(size_t(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload);
        std::memcpy(buffer.mapped, pixels, size_t(width) * height * 4);
        auto cmd = vk.beginOneTime();
        vk.transition(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {uint32_t(width), uint32_t(height), 1};
        vkCmdCopyBufferToImage(cmd, buffer.handle, texture.image.handle, texture.image.layout, 1, &copy);
        vk.transition(cmd, texture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        vk.endOneTime(cmd);
        vk.destroy(buffer);
        // One small pool per retained texture avoids a hard document/texture capacity in uiCore.
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        VK_CHECK(vkCreateDescriptorPool(vk.device, &pi, nullptr, &texture.pool));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = texture.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk.device, &ai, &texture.set));
        VkDescriptorImageInfo image{sampler, texture.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = texture.set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(vk.device, 1, &write, 0, nullptr);
    }
    void destroy(Texture& texture) {
        vk.destroy(texture.image);
        if (texture.pool)
            vkDestroyDescriptorPool(vk.device, texture.pool, nullptr);
    }
    ~Impl() {
        for (auto& pair : meshes) {
            vk.destroy(pair.second.vertices);
            vk.destroy(pair.second.indices);
        }
        for (auto& pair : textures)
            destroy(pair.second);
        destroy(white);
        vkDestroyPipeline(vk.device, pipeline, nullptr);
        vkDestroyPipelineLayout(vk.device, layout, nullptr);
        vkDestroyDescriptorSetLayout(vk.device, setLayout, nullptr);
        vkDestroySampler(vk.device, sampler, nullptr);
    }
    void prepare(const ui::UiFrame* frame) {
        for (auto it = meshes.begin(); it != meshes.end();) {
            if (it->second.source.expired()) {
                vk.destroy(it->second.vertices);
                vk.destroy(it->second.indices);
                it = meshes.erase(it);
            } else
                ++it;
        }
        for (auto it = textures.begin(); it != textures.end();) {
            if (it->second.source.expired()) {
                destroy(it->second);
                it = textures.erase(it);
            } else
                ++it;
        }
        if (!frame)
            return;
        for (const auto& draw : frame->draws) {
            const auto& g = *draw.geometry;
            if (meshes.find(g.id) == meshes.end()) {
                auto& mesh = meshes[g.id];
                mesh.source = draw.geometry;
                mesh.vertices = vk.buffer(g.vertices.size() * sizeof(ui::Vertex),
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, BufferMemory::Upload);
                mesh.indices =
                    vk.buffer(g.indices.size() * sizeof(int), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, BufferMemory::Upload);
                std::memcpy(mesh.vertices.mapped, g.vertices.data(), g.vertices.size() * sizeof(ui::Vertex));
                std::memcpy(mesh.indices.mapped, g.indices.data(), g.indices.size() * sizeof(int));
            }
            if (draw.texture && textures.find(draw.texture->id) == textures.end()) {
                auto& texture = textures[draw.texture->id];
                texture.source = draw.texture;
                upload(texture, draw.texture->width, draw.texture->height, draw.texture->pixels.data());
            }
        }
    }
    void draw(VkCommandBuffer command, Image& target, const ui::UiFrame* frame) {
        vk.transition(command, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = target.view;
        attachment.imageLayout = target.layout;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {target.width, target.height};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(command, &rendering);
        if (frame && frame->width > 0 && frame->height > 0) {
            VkViewport viewport{0, 0, float(target.width), float(target.height), 0, 1};
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            float sx = float(target.width) / frame->width, sy = float(target.height) / frame->height;
            for (const auto& draw : frame->draws) {
                int x = std::clamp(int(draw.scissor[0] * sx), 0, int(target.width));
                int y = std::clamp(int(draw.scissor[1] * sy), 0, int(target.height));
                int right = std::clamp(int((draw.scissor[0] + draw.scissor[2]) * sx), x, int(target.width));
                int bottom = std::clamp(int((draw.scissor[1] + draw.scissor[3]) * sy), y, int(target.height));
                if (x == right || y == bottom)
                    continue;
                VkRect2D scissor{{x, y}, {uint32_t(right - x), uint32_t(bottom - y)}};
                vkCmdSetScissor(command, 0, 1, &scissor);
                const auto& mesh = meshes.at(draw.geometry->id);
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(command, 0, 1, &mesh.vertices.handle, &offset);
                vkCmdBindIndexBuffer(command, mesh.indices.handle, 0, VK_INDEX_TYPE_UINT32);
                auto set = draw.texture ? textures.at(draw.texture->id).set : white.set;
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                        nullptr);
                Push push{draw.transform, draw.x, draw.y, float(frame->width), float(frame->height)};
                vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                vkCmdDrawIndexed(command, uint32_t(draw.geometry->indices.size()), 1, 0, 0, 0);
            }
        }
        vkCmdEndRendering(command);
        vk.transition(command, target, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
};
UiRenderer::UiRenderer(VulkanContext& vk) : impl_(std::make_unique<Impl>(vk)) {}
UiRenderer::~UiRenderer() = default;
void UiRenderer::prepare(const ui::UiFrame* frame) {
    impl_->prepare(frame);
}
void UiRenderer::draw(VkCommandBuffer command, Image& target, const ui::UiFrame* frame) {
    impl_->draw(command, target, frame);
}
} // namespace afterlight
