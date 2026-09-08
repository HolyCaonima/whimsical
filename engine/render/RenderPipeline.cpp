#include "RenderPipeline.h"
#include "NrdDenoiser.h"
#include "UiRenderer.h"
#include <fstream>

namespace afterlight {
using namespace rg;

namespace {
VkShaderModule createModule(VulkanContext& vk, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule shader;
    VK_CHECK(vkCreateShaderModule(vk.device, &info, nullptr, &shader));
    return shader;
}
VkShaderModule loadShader(VulkanContext& vk, const char* name) {
    std::string path = std::string(AFTERLIGHT_SHADERS) + "/" + name + ".spv";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Missing SPIR-V: " + path);
    size_t size = size_t(file.tellg());
    std::vector<uint32_t> data((size + 3) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
    return createModule(vk, data.data(), size);
}
VkClearColorValue black() {
    return {};
}
} // namespace

RenderPipeline::RenderPipeline(VulkanContext& vk, ShaderCompiler& shaders, ResourcePool& pool,
                               const RenderResources& resources)
    : vk_(vk), shaders_(shaders), pool_(pool), r_(resources) {
    compute_[Composite] = createCompute(loadShader(vk_, "composite.comp"));
    compute_[DiGradientFilter] = createCompute(loadShader(vk_, "di_gradient_filter.comp"));
    compute_[DiConfidence] = createCompute(loadShader(vk_, "di_confidence.comp"));
}

RenderPipeline::~RenderPipeline() {
    for (auto pass : {Composite, DiConfidence, DiGradientFilter})
        if (compute_[pass])
            vkDestroyPipeline(vk_.device, compute_[pass], nullptr);
    for (const auto& programs : computePrograms_)
        for (auto p : programs.second)
            vkDestroyPipeline(vk_.device, p, nullptr);
    for (const auto& program : rasterPrograms_)
        vkDestroyPipeline(vk_.device, program.second, nullptr);
}

VkPipeline RenderPipeline::createCompute(VkShaderModule module) {
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.layout = pool_.pipelineLayout();
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = module;
    cp.stage.pName = "main";
    VkPipeline pipeline;
    auto result = vkCreateComputePipelines(vk_.device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline);
    vkDestroyShaderModule(vk_.device, module, nullptr);
    VK_CHECK(result);
    return pipeline;
}

VkPipeline RenderPipeline::createRaster(const std::shared_ptr<const ShaderAsset>& shader) {
    const auto& code = shaders_.compile("gbuffer.frag", {shader});
    VkShaderModule vertex = loadShader(vk_, "gbuffer.vert"),
                   fragment = createModule(vk_, code.data(), code.size() * sizeof(uint32_t));
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = i ? fragment : vertex;
        stages[i].pName = "main";
    }
    VkVertexInputBindingDescription binding{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},  {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 32}, {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 48},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, 64},    {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 80}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 6;
    vi.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = shader->renderState.cull == SurfaceCull::Back    ? VK_CULL_MODE_BACK_BIT
                  : shader->renderState.cull == SurfaceCull::Front ? VK_CULL_MODE_FRONT_BIT
                                                                   : VK_CULL_MODE_NONE;
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
    gp.layout = pool_.pipelineLayout();
    VkPipeline raster;
    auto result = vkCreateGraphicsPipelines(vk_.device, VK_NULL_HANDLE, 1, &gp, nullptr, &raster);
    vkDestroyShaderModule(vk_.device, vertex, nullptr);
    vkDestroyShaderModule(vk_.device, fragment, nullptr);
    VK_CHECK(result);
    return raster;
}

void RenderPipeline::ensurePrograms(const MaterialBindings& bindings) {
    for (const auto& shader : bindings.shaders)
        if (!rasterPrograms_.count(shader))
            rasterPrograms_.emplace(shader, createRaster(shader));
    auto programs = computePrograms_.find(bindings.shaders);
    if (programs == computePrograms_.end()) {
        std::array<VkPipeline, ShaderCompiler::surfacePasses.size()> pipelines{};
        const auto& names = ShaderCompiler::surfacePasses;
        try {
            for (uint32_t i = 0; i < pipelines.size(); ++i) {
                const auto& code = shaders_.compile(names[i], bindings.shaders);
                pipelines[i] = createCompute(createModule(vk_, code.data(), code.size() * sizeof(uint32_t)));
            }
        } catch (...) {
            for (auto pipeline : pipelines)
                if (pipeline)
                    vkDestroyPipeline(vk_.device, pipeline, nullptr);
            throw;
        }
        programs = computePrograms_.emplace(bindings.shaders, pipelines).first;
    }
    std::copy(programs->second.begin(), programs->second.end(), compute_.begin());
}

void RenderPipeline::traceInputs(RenderGraph::Builder& pass) const {
    pass.read(r_.scene.shared(), Access::ComputeRead)
        .read(r_.scene.geometry(), Access::ComputeRead)
        .read(r_.scene.tlas, Access::TraceRead)
        .read(r_.gbuffer.surface(), Access::ComputeRead);
}

void RenderPipeline::build(RenderGraph& graph, const FrameSetup& setup) {
    const auto& scene = r_.scene;
    const auto& g = r_.gbuffer;
    const auto& di = r_.di;
    const auto& gi = r_.gi;
    const auto& shade = r_.shading;
    const Frame& frame = *setup.frame;
    graph.reset();

    if (setup.clearHistory) {
        auto resettable = pool_.registry().resettable();
        graph.add("Initialize History / Reservoirs")
            .write(resettable, Access::TransferWrite)
            .record([resettable](const PassContext& c) {
                VkClearColorValue clear{};
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                for (auto ref : resettable)
                    if (c.pool->declaration(ref.id).kind == Kind::Image)
                        vkCmdClearColorImage(c.command, c.image(ref).handle, c.image(ref).layout, &clear, 1,
                                             &range);
                    else
                        vkCmdFillBuffer(c.command, c.buffer(ref).handle, 0, VK_WHOLE_SIZE, 0);
            });
    }

    auto* ui = setup.ui;
    const auto* uiFrame = frame.ui.get();
    graph.add("RmlUi Overlay")
        .color(shade.hud, black())
        .record([ui, uiFrame](const PassContext& c) { ui->draw(c.command, c.width, c.height, uiFrame); });

    auto* gpuScene = setup.scene;
    if (gpuScene->skinsDirty())
        // The refit writes bottom-level structures the top-level build then consumes;
        // naming the structure the graph does know about produces exactly that edge.
        graph.add("Skinned BLAS Refit")
            .read(scene.geometry(), Access::BuildRead)
            .write(scene.tlas, Access::BuildWrite)
            .record([gpuScene](const PassContext& c) { gpuScene->recordSkinnedBlas(c.command); });

    auto* profiler = setup.profiler;
    graph.add("Acceleration Structures")
        .read(scene.instances, Access::BuildRead)
        .write(scene.tlas, Access::BuildWrite)
        .record([gpuScene, profiler](const PassContext& c) { gpuScene->recordTlas(c.command, *profiler); });

    VkClearColorValue farViewZ{};
    farViewZ.float32[0] = 10000;
    const auto& programs = rasterPrograms_;
    graph.add("GBuffer Raster")
        .color(g.albedo, black())
        .color(g.normal, black())
        .color(g.position, black())
        .color(g.motion, black())
        .color(g.viewZ, farViewZ)
        .color(g.emission, black())
        .depth(g.depth)
        .read(scene.shared(), Access::GraphicsRead)
        .read(scene.vertices, Access::VertexBuffer)
        .read(scene.indices, Access::IndexBuffer)
        .record([gpuScene, &frame, &programs](const PassContext& c) {
            vkCmdBindDescriptorSets(c.command, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1,
                                    &c.descriptors, 0, nullptr);
            gpuScene->recordDraws(c.command, frame, programs);
        });

    // The gradient replays last frame's selected sample against the current scene, so it
    // reads the previous G-buffer and the reservoirs before this frame overwrites them.
    auto gradient = graph.add("RTXDI Same Sample Gradient");
    traceInputs(gradient);
    gradient.read(g.previousSurface(), Access::ComputeRead)
        .read(previous(di.luminance), Access::ComputeRead)
        .read(di.reservoirs, Access::ComputeRead)
        .read(di.lightSamples, Access::ComputeRead)
        .write(di.gradient, Access::ComputeWrite)
        .dispatch(compute_[DiGradient], GradientDivisor);

    graph.add("RTXDI Gradient Filter")
        .read(previous(g.normal), Access::ComputeRead)
        .read(previous(g.viewZ), Access::ComputeRead)
        .read(di.gradient, Access::ComputeRead)
        .write(di.filteredGradient, Access::ComputeWrite)
        .dispatch(compute_[DiGradientFilter], GradientDivisor);

    graph.add("RTXDI History Confidence")
        .read(di.filteredGradient, Access::ComputeRead)
        .read(di.confidenceHistory, Access::ComputeRead)
        .write(di.diffuseConfidence, Access::ComputeWrite)
        .write(di.specularConfidence, Access::ComputeWrite)
        .dispatch(compute_[DiConfidence]);

    auto lighting = graph.add("RTXDI Initial + Secondary GI + Specular");
    traceInputs(lighting);
    lighting.read(di.lightSamples, Access::ComputeRead)
        .write(di.reservoirs, Access::ComputeWrite)
        .write(gi.candidate, Access::ComputeWrite)
        .write(shade.rawDiffuse, Access::ComputeWrite)
        .write(shade.rawSpecular, Access::ComputeWrite)
        .dispatch(compute_[Lighting]);

    auto temporal = graph.add("RTXDI Temporal Resampling");
    traceInputs(temporal);
    temporal.read(g.previousSurface(), Access::ComputeRead)
        .read(g.motion, Access::ComputeRead)
        .read(di.diffuseConfidence, Access::ComputeRead)
        .read(di.specularConfidence, Access::ComputeRead)
        .read(di.lightSamples, Access::ComputeRead)
        .write(di.reservoirs, Access::ComputeReadWrite)
        .dispatch(compute_[DiTemporal]);

    auto spatial = graph.add("RTXDI Spatial Resampling");
    traceInputs(spatial);
    spatial.read(di.neighbours, Access::ComputeRead)
        .read(di.lightSamples, Access::ComputeRead)
        .write(di.reservoirs, Access::ComputeReadWrite)
        .dispatch(compute_[DiSpatial]);

    auto reuse = graph.add("ReSTIR GI Reconnection");
    traceInputs(reuse);
    reuse.read(g.motion, Access::ComputeRead)
        .read(previous(g.position), Access::ComputeRead)
        .read(previous(g.normal), Access::ComputeRead)
        .read(gi.candidate, Access::ComputeRead)
        .read(previous(gi.reservoirs), Access::ComputeRead)
        .write(gi.reservoirs, Access::ComputeWrite)
        .dispatch(compute_[GiReuse]);

    auto resolve = graph.add("Visibility + Radiance Resolve");
    traceInputs(resolve);
    resolve.read(g.motion, Access::ComputeRead)
        .read(di.diffuseConfidence, Access::ComputeRead)
        .read(di.specularConfidence, Access::ComputeRead)
        .read(di.lightSamples, Access::ComputeRead)
        .read(gi.reservoirs, Access::ComputeRead)
        .write(di.reservoirs, Access::ComputeReadWrite)
        .write(di.confidenceHistory, Access::ComputeWrite)
        .write(di.luminance, Access::ComputeWrite)
        .write(shade.rawDiffuse, Access::ComputeReadWrite)
        .write(shade.rawSpecular, Access::ComputeReadWrite)
        .write(shade.directDebug, Access::ComputeWrite)
        .write(shade.indirectDebug, Access::ComputeWrite)
        .dispatch(compute_[Resolve]);

    auto* denoiser = setup.denoiser;
    const auto& camera = frame.camera;
    graph.add("NRD RELAX Diffuse Specular")
        .read(g.motion, Access::ComputeRead)
        .read(g.normal, Access::ComputeRead)
        .read(g.viewZ, Access::ComputeRead)
        .read(di.diffuseConfidence, Access::ComputeRead)
        .read(di.specularConfidence, Access::ComputeRead)
        .read(shade.rawDiffuse, Access::ComputeRead)
        .read(shade.rawSpecular, Access::ComputeRead)
        .write(shade.denoisedDiffuse, Access::ComputeWrite)
        .write(shade.denoisedSpecular, Access::ComputeWrite)
        .record([this, denoiser, &camera, profiler, index = uint32_t(setup.frameNumber),
                 reset = setup.reset, ms = setup.frameMs](const PassContext& c) {
            std::array<Image*, size_t(nrd::ResourceType::MAX_NUM)> resources{};
            auto bind = [&](nrd::ResourceType type, ResourceRef ref) {
                resources[size_t(type)] = &c.image(ref);
            };
            bind(nrd::ResourceType::IN_MV, r_.gbuffer.motion);
            bind(nrd::ResourceType::IN_NORMAL_ROUGHNESS, r_.gbuffer.normal);
            bind(nrd::ResourceType::IN_VIEWZ, r_.gbuffer.viewZ);
            bind(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, r_.shading.rawDiffuse);
            bind(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, r_.shading.rawSpecular);
            bind(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, r_.shading.denoisedDiffuse);
            bind(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, r_.shading.denoisedSpecular);
            bind(nrd::ResourceType::IN_DIFF_CONFIDENCE, r_.di.diffuseConfidence);
            bind(nrd::ResourceType::IN_SPEC_CONFIDENCE, r_.di.specularConfidence);
            denoiser->dispatch(c.command, resources, camera, index, reset, ms, *profiler);
        });

    graph.add("Composition + Tone Map + HUD")
        .read(scene.globals, Access::ComputeRead)
        .read(g.surface(), Access::ComputeRead)
        .read(g.motion, Access::ComputeRead)
        .read(shade.denoisedDiffuse, Access::ComputeRead)
        .read(shade.denoisedSpecular, Access::ComputeRead)
        .read(shade.rawDiffuse, Access::ComputeRead)
        .read(shade.rawSpecular, Access::ComputeRead)
        .read(shade.directDebug, Access::ComputeRead)
        .read(shade.indirectDebug, Access::ComputeRead)
        .read(shade.hud, Access::ComputeRead)
        .write(shade.display, Access::ComputeWrite)
        .dispatch(compute_[Composite]);

    if (setup.audit) {
        auto signals = setup.auditSignals;
        auto readback = r_.output.audit;
        graph.add("Audit Readback")
            .read(signals, Access::TransferRead)
            .write(readback, Access::TransferWrite)
            .sideEffect()
            .record([signals, readback](const PassContext& c) {
                for (uint32_t s = 0; s < signals.size(); ++s) {
                    auto& image = c.image(signals[s]);
                    VkBufferImageCopy copy{};
                    copy.bufferOffset = VkDeviceSize(s) * c.width * c.height * 8;
                    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copy.imageExtent = {c.width, c.height, 1};
                    vkCmdCopyImageToBuffer(c.command, image.handle, image.layout,
                                           c.buffer(readback).handle, 1, &copy);
                }
            });
    }

    // The display image is half float; the screenshot buffer is not. The blit is what
    // converts, so it needs a target of its own before the copy.
    if (setup.capture) {
        auto display = shade.display, image = r_.output.capture, buffer = r_.output.screenshot;
        graph.add("Screenshot Resolve")
            .read(display, Access::TransferRead)
            .write(image, Access::TransferWrite)
            .sideEffect()
            .record([display, image](const PassContext& c) {
                VkImageBlit region{};
                region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.srcOffsets[1] = {int32_t(c.width), int32_t(c.height), 1};
                region.dstOffsets[1] = {int32_t(c.width), int32_t(c.height), 1};
                vkCmdBlitImage(c.command, c.image(display).handle, c.image(display).layout,
                               c.image(image).handle, c.image(image).layout, 1, &region, VK_FILTER_NEAREST);
            });
        graph.add("Screenshot Readback")
            .read(image, Access::TransferRead)
            .write(buffer, Access::TransferWrite)
            .sideEffect()
            .record([image, buffer](const PassContext& c) {
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {c.width, c.height, 1};
                vkCmdCopyImageToBuffer(c.command, c.image(image).handle, c.image(image).layout,
                                       c.buffer(buffer).handle, 1, &copy);
            });
    }

    auto display = shade.display, swapchain = r_.output.swapchain;
    graph.add("Swapchain Blit")
        .read(display, Access::TransferRead)
        .write(swapchain, Access::TransferWrite)
        .sideEffect()
        .record([display, swapchain](const PassContext& c) {
            auto& target = c.image(swapchain);
            VkImageBlit region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[1] = {int32_t(c.width), int32_t(c.height), 1};
            region.dstOffsets[1] = {int32_t(target.width), int32_t(target.height), 1};
            vkCmdBlitImage(c.command, c.image(display).handle, c.image(display).layout, target.handle,
                           target.layout, 1, &region, VK_FILTER_NEAREST);
        });
    graph.add("Present Transition").read(swapchain, Access::Present).sideEffect();
}
} // namespace afterlight
