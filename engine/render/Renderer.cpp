#include "Renderer.h"
#include "VulkanContext.h"
#include "NrdDenoiser.h"
#include "RenderGraph.h"
#include "Geometry.h"
#include "DebugHud.h"
#include "RenderAudit.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <algorithm>
namespace afterlight {
namespace {
constexpr uint32_t MaxInstances = 1024, MaxLights = 256, MaxMaterials = 256;
constexpr VkImageUsageFlags ColorUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
struct alignas(16) Globals {
    mat4 vp, previousVp, view, inverseVp;
    vec4 eyeTime, resolution, player, destination;
    glm::uvec4 counts;
};
struct alignas(16) GpuInstance {
    mat4 model, previousModel;
    glm::uvec4 info;
};
static_assert(sizeof(Globals) == 336 && sizeof(GpuInstance) == 144 && sizeof(Material) == 32 &&
                  sizeof(GpuVertex) == 32,
              "GPU layout mismatch");
struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer storage;
    VkDeviceAddress address = 0;
};
VkShaderModule loadShader(VulkanContext& vk, const char* name) {
    std::string path = std::string(AFTERLIGHT_SHADERS) + "/" + name + ".spv";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Missing SPIR-V: " + path);
    size_t size = size_t(file.tellg());
    std::vector<uint32_t> data((size + 3) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = data.data();
    VkShaderModule shader;
    VK_CHECK(vkCreateShaderModule(vk.device, &info, nullptr, &shader));
    return shader;
}
} // namespace
struct Renderer::Impl {
    VulkanContext vk;
    RenderOptions options;
    HWND window;
    uint32_t width = 0, height = 0;
    uint64_t frameNumber = 0;
    double gpuMs = 16.7;
    RenderStatistics statistics;
    std::chrono::steady_clock::time_point statisticsStart{};
    uint32_t statisticsIntervals = 0;
    double statisticsGpuSum = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<Image> swapImages;
    std::vector<VkSemaphore> finished;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline raster = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> compute{};
    Buffer globals, instanceData, materialData, lightData, vertexData, indexData, tlasInstances, tlasScratch,
        hudUpload, readback, auditReadback;
    RenderAudit audit;
    std::array<Buffer, 6> reservoirs;
    std::array<AccelerationStructure, 2> blas;
    AccelerationStructure tlas;
    uint32_t tlasCount = 0;
    std::array<MeshRange, 2> meshes;
    uint32_t vertexCount = 0;
    // Binding number -> image; unused bindings intentionally remain empty.
    std::array<Image, 29> images;
    Image depth, captureImage;
    std::unique_ptr<NrdDenoiser> denoiser;
    DebugHud hud;
    FrameRef previous;
    bool historyValid = false;
    bool resizePending = false;
    std::chrono::steady_clock::time_point previousRenderTime = std::chrono::steady_clock::now();
    Impl(HWND hwnd, const RenderOptions& opts) : options(opts), window(hwnd) {}
    ~Impl() {
        if (!vk.device)
            return;
        vkDeviceWaitIdle(vk.device);
        denoiser.reset();
        hud.reset();
        for (auto& b : reservoirs)
            vk.destroy(b);
        for (auto& i : images)
            vk.destroy(i);
        vk.destroy(depth);
        vk.destroy(captureImage);
        for (auto b : {&globals, &instanceData, &materialData, &lightData, &vertexData, &indexData,
                       &tlasInstances, &tlasScratch, &hudUpload, &readback, &auditReadback})
            vk.destroy(*b);
        for (auto& a : blas)
            destroyAS(a);
        destroyAS(tlas);
        for (auto s : finished)
            vkDestroySemaphore(vk.device, s, nullptr);
        if (swapchain)
            vkDestroySwapchainKHR(vk.device, swapchain, nullptr);
        if (acquired)
            vkDestroySemaphore(vk.device, acquired, nullptr);
        if (fence)
            vkDestroyFence(vk.device, fence, nullptr);
        if (timestamps)
            vkDestroyQueryPool(vk.device, timestamps, nullptr);
        for (auto p : compute)
            if (p)
                vkDestroyPipeline(vk.device, p, nullptr);
        if (raster)
            vkDestroyPipeline(vk.device, raster, nullptr);
        if (pipelineLayout)
            vkDestroyPipelineLayout(vk.device, pipelineLayout, nullptr);
        if (descriptorPool)
            vkDestroyDescriptorPool(vk.device, descriptorPool, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(vk.device, setLayout, nullptr);
    }
    void destroyAS(AccelerationStructure& a) {
        if (a.handle)
            vkDestroyAccelerationStructureKHR(vk.device, a.handle, nullptr);
        vk.destroy(a.storage);
        a = {};
    }
    AccelerationStructure createAS(VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
        AccelerationStructure a;
        a.storage = vk.buffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = a.storage.handle;
        ci.size = size;
        ci.type = type;
        VK_CHECK(vkCreateAccelerationStructureKHR(vk.device, &ci, nullptr, &a.handle));
        VkAccelerationStructureDeviceAddressInfoKHR address{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        address.accelerationStructure = a.handle;
        a.address = vkGetAccelerationStructureDeviceAddressKHR(vk.device, &address);
        return a;
    }
    VkDeviceAddress alignedScratch(const Buffer& buffer) {
        VkDeviceSize alignment = vk.asProperties.minAccelerationStructureScratchOffsetAlignment;
        return (buffer.address + alignment - 1) / alignment * alignment;
    }
    void initialize() {
        vk.initialize(window, options.validation);
        globals = vk.buffer(sizeof(Globals), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        instanceData =
            vk.buffer(sizeof(GpuInstance) * MaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        materialData = vk.buffer(sizeof(Material) * MaxMaterials, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        lightData = vk.buffer(sizeof(Light) * MaxLights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        tlasInstances = vk.buffer(sizeof(VkAccelerationStructureInstanceKHR) * MaxInstances,
                                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  true);
        std::vector<GpuVertex> vertices;
        std::vector<uint32_t> indices;
        meshes = buildPrimitives(vertices, indices);
        vertexCount = uint32_t(vertices.size());
        auto geometryUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        vertexData = vk.buffer(vertices.size() * sizeof(GpuVertex),
                               geometryUsage | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        indexData = vk.buffer(indices.size() * sizeof(uint32_t),
                              geometryUsage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        std::memcpy(vertexData.mapped, vertices.data(), size_t(vertexData.size));
        std::memcpy(indexData.mapped, indices.data(), size_t(indexData.size));
        for (uint32_t m = 0; m < 2; m++) {
            VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            geom.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
            auto& tri = geom.geometry.triangles;
            tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            tri.vertexData.deviceAddress = vertexData.address;
            tri.vertexStride = sizeof(GpuVertex);
            tri.maxVertex = vertexCount - 1;
            tri.indexType = VK_INDEX_TYPE_UINT32;
            tri.indexData.deviceAddress = indexData.address + meshes[m].firstIndex * sizeof(uint32_t);
            VkAccelerationStructureBuildGeometryInfoKHR build{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.geometryCount = 1;
            build.pGeometries = &geom;
            uint32_t primitives = meshes[m].indexCount / 3;
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(
                vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes);
            blas[m] = createAS(build.type, sizes.accelerationStructureSize);
            auto scratch = vk.buffer(
                sizes.buildScratchSize + vk.asProperties.minAccelerationStructureScratchOffsetAlignment,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            build.dstAccelerationStructure = blas[m].handle;
            build.scratchData.deviceAddress = alignedScratch(scratch);
            VkAccelerationStructureBuildRangeInfoKHR range{primitives, 0, 0, 0};
            const auto* rangePointer = &range;
            auto c = vk.beginOneTime();
            vkCmdBuildAccelerationStructuresKHR(c, 1, &build, &rangePointer);
            vk.endOneTime(c);
            vk.destroy(scratch);
        }
        createPipelines();
        denoiser = std::make_unique<NrdDenoiser>(vk);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(vk.device, &si, nullptr, &acquired));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(vk.device, &fi, nullptr, &fence));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = vk.commandPool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(vk.device, &ca, &command));
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        VK_CHECK(vkCreateQueryPool(vk.device, &qi, nullptr, &timestamps));
    }
    void createPipelines() {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (uint32_t b = 0; b <= 28; b++) {
            VkDescriptorType type = b == 0                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                    : b <= 5 || (b >= 13 && b <= 18) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                    : b == 6 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                             : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings.push_back(
                {b, type, 1, VkShaderStageFlags(b <= 3 ? VK_SHADER_STAGE_ALL : VK_SHADER_STAGE_COMPUTE_BIT),
                 nullptr});
        }
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = uint32_t(bindings.size());
        sl.pBindings = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &sl, nullptr, &setLayout));
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        VK_CHECK(vkCreatePipelineLayout(vk.device, &pl, nullptr, &pipelineLayout));
        VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11},
                                            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1;
        dp.poolSizeCount = 4;
        dp.pPoolSizes = poolSizes;
        VK_CHECK(vkCreateDescriptorPool(vk.device, &dp, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptorPool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &setLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk.device, &da, &descriptors));
        const char* names[] = {"lighting.comp", "reuse.comp", "resolve.comp", "composite.comp"};
        for (uint32_t i = 0; i < 4; i++) {
            auto module = loadShader(vk, names[i]);
            VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cp.layout = pipelineLayout;
            cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cp.stage.module = module;
            cp.stage.pName = "main";
            auto result = vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &cp, nullptr, &compute[i]);
            vkDestroyShaderModule(vk.device, module, nullptr);
            VK_CHECK(result);
        }
        VkShaderModule vertex = loadShader(vk, "gbuffer.vert"), fragment = loadShader(vk, "gbuffer.frag");
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (int i = 0; i < 2; i++) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = i ? fragment : vertex;
            stages[i].pName = "main";
        }
        VkVertexInputBindingDescription binding{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                          {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        std::array<VkPipelineColorBlendAttachmentState, 6> attachments{};
        for (auto& a : attachments)
            a.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = uint32_t(attachments.size());
        blend.pAttachments = attachments.data();
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = states;
        VkFormat formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_FORMAT_R32_SFLOAT,          VK_FORMAT_R16G16B16A16_SFLOAT};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 6;
        rendering.pColorAttachmentFormats = formats;
        rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &rendering;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &viewport;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &blend;
        gp.pDynamicState = &dynamic;
        gp.layout = pipelineLayout;
        auto result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &gp, nullptr, &raster);
        vkDestroyShaderModule(vk.device, vertex, nullptr);
        vkDestroyShaderModule(vk.device, fragment, nullptr);
        VK_CHECK(result);
    }
    static const char* presentName(VkPresentModeKHR mode) {
        return mode == VK_PRESENT_MODE_MAILBOX_KHR     ? "MAILBOX"
               : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
                                                       : "FIFO";
    }
    // FIFO is the only mode Vulkan guarantees, so an unsupported request degrades to it
    // rather than failing to start.
    VkPresentModeKHR selectPresentMode() const {
        VkPresentModeKHR wanted = options.present == PresentMode::Mailbox ? VK_PRESENT_MODE_MAILBOX_KHR
                                  : options.present == PresentMode::Immediate
                                      ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                      : VK_PRESENT_MODE_FIFO_KHR;
        if (wanted == VK_PRESENT_MODE_FIFO_KHR)
            return wanted;
        uint32_t count = 0;
        VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &count, nullptr));
        std::vector<VkPresentModeKHR> modes(count);
        VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &count, modes.data()));
        if (std::find(modes.begin(), modes.end(), wanted) != modes.end())
            return wanted;
        std::cout << "[Render] " << presentName(wanted) << " present mode unsupported; using FIFO\n";
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    void resize(uint32_t requestedWidth, uint32_t requestedHeight) {
        VK_CHECK(vkDeviceWaitIdle(vk.device));
        VkSurfaceCapabilitiesKHR caps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &caps));
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw std::runtime_error("Swapchain does not support transfer destination");
        uint32_t n = 0;
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &n, nullptr));
        std::vector<VkSurfaceFormatKHR> formats(n);
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &n, formats.data()));
        auto format = formats.front();
        for (auto f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                format = f;
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX)
            extent = {std::clamp(requestedWidth, caps.minImageExtent.width, caps.maxImageExtent.width),
                      std::clamp(requestedHeight, caps.minImageExtent.height, caps.maxImageExtent.height)};
        width = extent.width;
        height = extent.height;
        uint32_t count = std::max(3u, caps.minImageCount);
        if (caps.maxImageCount)
            count = std::min(count, caps.maxImageCount);
        VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sc.surface = vk.surface;
        sc.minImageCount = count;
        sc.imageFormat = format.format;
        sc.imageColorSpace = format.colorSpace;
        sc.imageExtent = extent;
        sc.imageArrayLayers = 1;
        sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sc.preTransform = caps.currentTransform;
        sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        presentMode = selectPresentMode();
        sc.presentMode = presentMode;
        sc.clipped = VK_TRUE;
        sc.oldSwapchain = swapchain;
        VkSwapchainKHR next;
        VK_CHECK(vkCreateSwapchainKHR(vk.device, &sc, nullptr, &next));
        if (swapchain)
            vkDestroySwapchainKHR(vk.device, swapchain, nullptr);
        swapchain = next;
        for (auto s : finished)
            vkDestroySemaphore(vk.device, s, nullptr);
        finished.clear();
        VK_CHECK(vkGetSwapchainImagesKHR(vk.device, swapchain, &count, nullptr));
        std::vector<VkImage> handles(count);
        VK_CHECK(vkGetSwapchainImagesKHR(vk.device, swapchain, &count, handles.data()));
        swapImages.clear();
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto handle : handles) {
            Image i;
            i.handle = handle;
            i.format = format.format;
            i.width = width;
            i.height = height;
            swapImages.push_back(i);
            VkSemaphore semaphore;
            VK_CHECK(vkCreateSemaphore(vk.device, &semaphoreInfo, nullptr, &semaphore));
            finished.push_back(semaphore);
        }
        for (auto& i : images)
            vk.destroy(i);
        vk.destroy(depth);
        vk.destroy(captureImage);
        for (auto& b : reservoirs)
            vk.destroy(b);
        vk.destroy(hudUpload);
        vk.destroy(readback);
        vk.destroy(auditReadback);
        audit = {};
        for (uint32_t b = 7; b <= 28; b++)
            if (!(b >= 13 && b <= 18)) {
                VkFormat imageFormat = b == 9 || b == 25 ? VK_FORMAT_R32G32B32A32_SFLOAT
                                       : b == 11         ? VK_FORMAT_R32_SFLOAT
                                       : b == 28         ? VK_FORMAT_R8G8B8A8_UNORM
                                                         : VK_FORMAT_R16G16B16A16_SFLOAT;
                images[b] = vk.image(width, height, imageFormat,
                                     ColorUsage | (b <= 12 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0));
            }
        depth = vk.image(width, height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        captureImage = vk.image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT);
        VkDeviceSize pixels = VkDeviceSize(width) * height;
        for (uint32_t i = 0; i < 6; i++)
            reservoirs[i] = vk.buffer(pixels * (i < 3 ? 32 : 64), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        hudUpload = vk.buffer(pixels * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        readback = vk.buffer(pixels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        if (!options.audit.empty())
            auditReadback = vk.buffer(pixels * 8 * 3, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        hud.resize(width, height);
        denoiser->resize(width, height);
        historyValid = false;
        resizePending = false;
        statistics = {};
        statistics.present = presentName(presentMode);
        statistics.vsync = presentMode == VK_PRESENT_MODE_FIFO_KHR;
        statisticsStart = {};
        statisticsIntervals = 0;
        statisticsGpuSum = 0;
    }
    void updateDescriptors() {
        std::array<VkDescriptorBufferInfo, 29> buffers{};
        std::array<VkDescriptorImageInfo, 29> textureInfo{};
        std::vector<VkWriteDescriptorSet> writes;
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &tlas.handle;
        Buffer* basics[] = {&globals, &instanceData, &materialData, &lightData, &vertexData, &indexData};
        for (uint32_t b = 0; b <= 28; b++) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = descriptors;
            w.dstBinding = b;
            w.descriptorCount = 1;
            if (b <= 5 || (b >= 13 && b <= 18)) {
                auto* buffer = b <= 5 ? basics[b] : &reservoirs[b - 13];
                buffers[b] = {buffer->handle, 0, buffer->size};
                w.descriptorType =
                    b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.pBufferInfo = &buffers[b];
            } else if (b == 6) {
                w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                w.pNext = &asWrite;
            } else {
                textureInfo[b] = {VK_NULL_HANDLE, images[b].view, VK_IMAGE_LAYOUT_GENERAL};
                w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w.pImageInfo = &textureInfo[b];
            }
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(vk.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    }
    void buildTLAS(VkCommandBuffer c, const Frame& frame) {
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        geometry.geometry.instances.data.deviceAddress = tlasInstances.address;
        VkAccelerationStructureBuildGeometryInfoKHR info{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                     VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        uint32_t count = uint32_t(frame.entities.size());
        bool update = tlas.handle && tlasCount == count;
        if (!update) {
            destroyAS(tlas);
            vk.destroy(tlasScratch);
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(
                vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
            tlas = createAS(info.type, sizes.accelerationStructureSize);
            tlasScratch =
                vk.buffer(std::max(sizes.buildScratchSize, sizes.updateScratchSize) +
                              vk.asProperties.minAccelerationStructureScratchOffsetAlignment,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            tlasCount = count;
        }
        info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                           : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.srcAccelerationStructure = update ? tlas.handle : VK_NULL_HANDLE;
        info.dstAccelerationStructure = tlas.handle;
        info.scratchData.deviceAddress = alignedScratch(tlasScratch);
        VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
        const auto* rangePointer = &range;
        vkCmdBuildAccelerationStructuresKHR(c, 1, &info, &rangePointer);
    }
    void rasterize(VkCommandBuffer c, const Frame& frame) {
        std::array<VkRenderingAttachmentInfo, 6> attachments{};
        for (uint32_t i = 0; i < 6; i++) {
            auto& image = images[7 + i];
            vk.transition(c, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            auto& a = attachments[i];
            a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            a.imageView = image.view;
            a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (i == 4)
                a.clearValue.color.float32[0] = 10000;
        }
        vk.transition(c, depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = depth.view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1, 0};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 6;
        rendering.pColorAttachments = attachments.data();
        rendering.pDepthAttachment = &depthAttachment;
        vkCmdBeginRendering(c, &rendering);
        VkViewport viewport{0, 0, float(width), float(height), 0, 1};
        VkRect2D scissor{{0, 0}, {width, height}};
        vkCmdSetViewport(c, 0, 1, &viewport);
        vkCmdSetScissor(c, 0, 1, &scissor);
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, raster);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptors, 0,
                                nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(c, 0, 1, &vertexData.handle, &offset);
        vkCmdBindIndexBuffer(c, indexData.handle, 0, VK_INDEX_TYPE_UINT32);
        for (uint32_t i = 0; i < frame.entities.size(); i++)
            if (frame.entities[i].enabled) {
                auto& mesh = meshes[uint32_t(frame.entities[i].shape)];
                vkCmdDrawIndexed(c, mesh.indexCount, 1, mesh.firstIndex, 0, i);
            }
        vkCmdEndRendering(c);
        for (uint32_t b = 7; b <= 12; b++)
            vk.transition(c, images[b], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    void dispatch(VkCommandBuffer c, uint32_t pipeline) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, compute[pipeline]);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptors, 0,
                                nullptr);
        vkCmdDispatch(c, (width + 7) / 8, (height + 7) / 8, 1);
    }
    void copyHistory(VkCommandBuffer c) {
        for (auto pair : {std::pair<uint32_t, uint32_t>{8, 24}, {9, 25}}) {
            auto& source = images[pair.first];
            auto& destination = images[pair.second];
            vk.transition(c, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vk.transition(c, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {width, height, 1};
            vkCmdCopyImage(c, source.handle, source.layout, destination.handle, destination.layout, 1,
                           &region);
            vk.transition(c, source, VK_IMAGE_LAYOUT_GENERAL);
            vk.transition(c, destination, VK_IMAGE_LAYOUT_GENERAL);
        }
        for (uint32_t i : {0u, 3u}) {
            VkBufferCopy copy{0, 0, reservoirs[i].size};
            vkCmdCopyBuffer(c, reservoirs[i].handle, reservoirs[i + 1].handle, 1, &copy);
        }
    }
    void blit(VkCommandBuffer c, Image& source, Image& destination) {
        vk.transition(c, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk.transition(c, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[1] = {int32_t(source.width), int32_t(source.height), 1};
        region.dstOffsets[1] = {int32_t(destination.width), int32_t(destination.height), 1};
        vkCmdBlitImage(c, source.handle, source.layout, destination.handle, destination.layout, 1, &region,
                       VK_FILTER_NEAREST);
    }
    void saveCapture() {
        std::filesystem::path dir = std::filesystem::path(AFTERLIGHT_ROOT) / "captures";
        if (!options.audit.empty())
            dir /= options.audit;
        std::filesystem::create_directories(dir);
        auto* rgba = static_cast<uint8_t*>(readback.mapped);
        std::vector<uint8_t> bgra(size_t(width) * height * 4);
        uint64_t sum = 0;
        uint8_t minimum = 255, maximum = 0;
        for (size_t p = 0; p < size_t(width) * height; p++) {
            bgra[p * 4] = rgba[p * 4 + 2];
            bgra[p * 4 + 1] = rgba[p * 4 + 1];
            bgra[p * 4 + 2] = rgba[p * 4];
            bgra[p * 4 + 3] = 255;
            sum += rgba[p * 4] + rgba[p * 4 + 1] + rgba[p * 4 + 2];
            minimum = std::min(minimum, rgba[p * 4]);
            maximum = std::max(maximum, rgba[p * 4]);
        }
        BITMAPFILEHEADER file{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + DWORD(bgra.size());
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = LONG(width);
        info.biHeight = -LONG(height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        std::ofstream output(dir / "frame.bmp", std::ios::binary);
        output.write(reinterpret_cast<char*>(&file), sizeof(file));
        output.write(reinterpret_cast<char*>(&info), sizeof(info));
        output.write(reinterpret_cast<char*>(bgra.data()), std::streamsize(bgra.size()));
        std::ofstream report(dir / "render-report.json");
        report << "{\n  \"gpu\": \"" << vk.properties.deviceName << "\",\n  \"frames\": " << frameNumber
               << ",\n  \"width\": " << width << ", \"height\": " << height << ",\n  \"gpuMs\": " << gpuMs
               << ",\n  \"fps\": " << statistics.fps << ",\n  \"frameMs\": " << statistics.frameMs
               << ",\n  \"gpuAverageMs\": " << statistics.gpuMs
               << ",\n  \"simulationHz\": 60,\n  \"presentMode\": \"" << presentName(presentMode) << "\""
               << ",\n  \"validationActive\": " << (vk.validationActive ? "true" : "false")
               << ",\n  \"validationErrors\": " << vk.validationErrors.load()
               << ",\n  \"meanRgb\": " << double(sum) / (double(width) * height * 3) << ",\n  \"redRange\": ["
               << int(minimum) << "," << int(maximum) << "]\n}\n";
        std::cout << "Capture: " << (dir / "frame.bmp").string() << " | " << statistics.fps << " FPS | Frame "
                  << statistics.frameMs << " ms | GPU " << statistics.gpuMs << " ms\n";
    }
    void updateStatistics() {
        const auto now = std::chrono::steady_clock::now();
        if (statisticsStart == std::chrono::steady_clock::time_point{}) {
            statisticsStart = now;
            return;
        }
        ++statisticsIntervals;
        statisticsGpuSum += gpuMs;
        const double elapsed = std::chrono::duration<double>(now - statisticsStart).count();
        if (elapsed >= .5) {
            // Count actual frame intervals, including snapshot, GPU and present waits.
            // GPU duration alone is not complete frame time and cannot be inverted for FPS.
            statistics.fps = statisticsIntervals / elapsed;
            statistics.frameMs = elapsed * 1000 / statisticsIntervals;
            statistics.gpuMs = statisticsGpuSum / statisticsIntervals;
            statisticsStart = now;
            statisticsIntervals = 0;
            statisticsGpuSum = 0;
        }
    }
    bool render(const FrameRef& sourceFrame) {
        FrameRef frameRef = sourceFrame;
        if (options.auditMotion) {
            Frame diagnostic = *sourceFrame;
            diagnostic.camera.yaw += .04f * std::sin(float(frameNumber) * .017f);
            frameRef = std::make_shared<const Frame>(std::move(diagnostic));
        }
        const Frame& frame = *frameRef;
        if (!frame.input.width || !frame.input.height)
            return true;
        if (frame.entities.empty() || frame.entities.size() > MaxInstances ||
            frame.materials.size() > MaxMaterials || frame.lights.empty() || frame.lights.size() > MaxLights)
            throw std::runtime_error("Scene capacity exceeded or scene has no lights/instances");
        VK_CHECK(vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX));
        if (frameNumber) {
            uint64_t values[2]{};
            if (vkGetQueryPoolResults(vk.device, timestamps, 0, 2, sizeof(values), values, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                gpuMs = double(values[1] - values[0]) * vk.properties.limits.timestampPeriod / 1e6;
        }
        if (width != frame.input.width || height != frame.input.height || resizePending)
            resize(frame.input.width, frame.input.height);
        uint32_t swapIndex;
        auto acquire =
            vkAcquireNextImageKHR(vk.device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &swapIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            resizePending = true;
            return true;
        }
        if (acquire != VK_SUBOPTIMAL_KHR)
            VK_CHECK(acquire);
        else
            resizePending = true;
        auto now = std::chrono::steady_clock::now();
        float frameMs = std::clamp(
            float(std::chrono::duration<double, std::milli>(now - previousRenderTime).count()), 1.f, 100.f);
        previousRenderTime = now;
        bool reset = !historyValid || frame.resetHistory ||
                     glm::distance(frame.camera.eye(), previous->camera.eye()) > 4.f;
        if (historyValid) {
            if (frame.lights.size() != previous->lights.size() ||
                frame.materials.size() != previous->materials.size())
                reset = true;
            else if (std::memcmp(frame.lights.data(), previous->lights.data(),
                                 frame.lights.size() * sizeof(Light)) ||
                     std::memcmp(frame.materials.data(), previous->materials.data(),
                                 frame.materials.size() * sizeof(Material)))
                reset = true;
        }
        Globals data{};
        data.view = frame.camera.view();
        data.vp = frame.camera.projection(float(width) / height) * data.view;
        data.previousVp =
            reset ? data.vp : previous->camera.projection(float(width) / height) * previous->camera.view();
        data.inverseVp = glm::inverse(data.vp);
        data.eyeTime = vec4(frame.camera.eye(), float(frame.time));
        data.resolution = {float(width), float(height), float(frame.debugView), float(frame.hovered)};
        data.player = vec4(frame.player, float(frame.selected));
        data.destination = vec4(frame.destination, frame.hasDestination ? 1.f : 0.f);
        data.counts = {uint32_t(frame.entities.size()), uint32_t(frame.lights.size()), uint32_t(frameNumber),
                       reset ? 0u : 1u};
        std::memcpy(globals.mapped, &data, sizeof(data));
        auto* gpuInstances = static_cast<GpuInstance*>(instanceData.mapped);
        auto* accelerationInstances = static_cast<VkAccelerationStructureInstanceKHR*>(tlasInstances.mapped);
        for (uint32_t i = 0; i < frame.entities.size(); i++) {
            auto& entity = frame.entities[i];
            auto model = transform(entity);
            auto previousModel = model;
            if (!reset && i < previous->entities.size() && previous->entities[i].id == entity.id)
                previousModel = transform(previous->entities[i]);
            gpuInstances[i] = {model,
                               previousModel,
                               {entity.material, meshes[uint32_t(entity.shape)].firstIndex, entity.id,
                                entity.interactable ? 1u : 0u}};
            VkAccelerationStructureInstanceKHR a{};
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++)
                    a.transform.matrix[r][c] = model[c][r];
            a.instanceCustomIndex = i;
            a.mask = entity.enabled ? 0xff : 0;
            a.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            a.accelerationStructureReference = blas[uint32_t(entity.shape)].address;
            accelerationInstances[i] = a;
        }
        std::memcpy(materialData.mapped, frame.materials.data(), frame.materials.size() * sizeof(Material));
        std::memcpy(lightData.mapped, frame.lights.data(), frame.lights.size() * sizeof(Light));
        // A history reset clears every screen image, including the overlay, so the upload
        // has to be replayed even when the overlay content itself did not change.
        const bool hudDirty = hud.draw(frame, hudUpload.mapped, statistics, options.hud) || !historyValid;
        VK_CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command, &begin));
        vkCmdResetQueryPool(command, timestamps, 0, 2);
        vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps, 0);
        if (!historyValid) {
            VkClearColorValue clear{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            for (auto& image : images)
                if (image.handle) {
                    vk.transition(command, image, VK_IMAGE_LAYOUT_GENERAL);
                    vkCmdClearColorImage(command, image.handle, image.layout, &clear, 1, &range);
                }
            for (auto& buffer : reservoirs)
                vkCmdFillBuffer(command, buffer.handle, 0, VK_WHOLE_SIZE, 0);
            VulkanContext::barrier(command);
        }
        if (hudDirty) {
            vk.transition(command, images[28], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy upload{};
            upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            upload.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(command, hudUpload.handle, images[28].handle, images[28].layout, 1,
                                   &upload);
            vk.transition(command, images[28], VK_IMAGE_LAYOUT_GENERAL);
        }
        // TLAS allocation precedes descriptor writes; actual construction is recorded before raster/compute.
        buildTLAS(command, frame);
        VulkanContext::barrier(command);
        updateDescriptors();
        RenderGraph graph;
        graph.add("GBuffer Raster", [&](auto c) { rasterize(c, frame); });
        graph.add("ReSTIR Initial DI + Secondary GI + Specular", [&](auto c) { dispatch(c, 0); });
        graph.add("ReSTIR Temporal + Spatial Reconnection", [&](auto c) { dispatch(c, 1); });
        graph.add("Visibility + Radiance Resolve", [&](auto c) { dispatch(c, 2); });
        graph.add("NRD RELAX Diffuse Specular", [&](auto c) {
            std::array<Image*, size_t(nrd::ResourceType::MAX_NUM)> resources{};
            resources[size_t(nrd::ResourceType::IN_MV)] = &images[10];
            resources[size_t(nrd::ResourceType::IN_NORMAL_ROUGHNESS)] = &images[8];
            resources[size_t(nrd::ResourceType::IN_VIEWZ)] = &images[11];
            resources[size_t(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST)] = &images[19];
            resources[size_t(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST)] = &images[20];
            resources[size_t(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST)] = &images[21];
            resources[size_t(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST)] = &images[22];
            denoiser->dispatch(c, resources, frame.camera, uint32_t(frameNumber), reset, frameMs);
        });
        graph.add("Composition + Tone Map + HUD", [&](auto c) { dispatch(c, 3); });
        graph.add("History Store", [&](auto c) { copyHistory(c); });
        graph.execute(command);
        bool auditFrame = !options.audit.empty() && frameNumber >= 64;
        if (auditFrame) {
            const uint32_t bindings[] = {26, 7, 23};
            for (uint32_t s = 0; s < 3; ++s) {
                auto& image = images[bindings[s]];
                vk.transition(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy copy{};
                copy.bufferOffset = VkDeviceSize(s) * width * height * 8;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {width, height, 1};
                vkCmdCopyImageToBuffer(command, image.handle, image.layout, auditReadback.handle, 1, &copy);
                vk.transition(command, image, VK_IMAGE_LAYOUT_GENERAL);
            }
        }
        bool last = options.maxFrames && frameNumber + 1 >= options.maxFrames;
        if (options.capture && last) {
            blit(command, images[23], captureImage);
            vk.transition(command, captureImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            vkCmdCopyImageToBuffer(command, captureImage.handle, captureImage.layout, readback.handle, 1,
                                   &copy);
        }
        blit(command, images[23], swapImages[swapIndex]);
        vk.transition(command, swapImages[swapIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_2_NONE, 0);
        vk.transition(command, images[23], VK_IMAGE_LAYOUT_GENERAL);
        vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps, 1);
        VK_CHECK(vkEndCommandBuffer(command));
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &finished[swapIndex];
        VK_CHECK(vkResetFences(vk.device, 1, &fence));
        VK_CHECK(vkQueueSubmit(vk.queue, 1, &submit, fence));
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &finished[swapIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &swapIndex;
        auto result = vkQueuePresentKHR(vk.queue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            resizePending = true;
        else
            VK_CHECK(result);
        previous = frameRef;
        historyValid = true;
        frameNumber++;
        if (auditFrame) {
            VK_CHECK(vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX));
            audit.add(static_cast<const uint16_t*>(auditReadback.mapped), size_t(width) * height);
            if (last)
                audit.save(std::filesystem::path(AFTERLIGHT_ROOT) / "captures" / options.audit, width,
                           height);
        }
        updateStatistics();
        if (last) {
            VK_CHECK(vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX));
            if (options.capture)
                saveCapture();
            return false;
        }
        return true;
    }
};
Renderer::Renderer(HWND window, const RenderOptions& options)
    : impl_(std::make_unique<Impl>(window, options)) {
    impl_->initialize();
}
Renderer::~Renderer() = default;
bool Renderer::render(const FrameRef& f) {
    return impl_->render(f);
}
uint64_t Renderer::frames() const {
    return impl_->frameNumber;
}
RenderStatistics Renderer::statistics() const {
    return impl_->statistics;
}
uint32_t Renderer::errors() const {
    return impl_->vk.validationErrors.load();
}
} // namespace afterlight
