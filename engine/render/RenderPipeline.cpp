#include "RenderPipeline.h"
#include "NrdDenoiser.h"
#include "UiRenderer.h"
#include "GpuRenderTargets.h"
#include "graph/ImageReadback.h"
#include <fstream>

namespace whimsical {
using namespace rg;

namespace {
VkShaderModule createModule(VulkanContext& vk, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule shader;
    VK_CHECK(vkCreateShaderModule(vk.device, &info, nullptr, &shader));
    return shader;
}
// The SPIR-V is both what runs and what says which resources the pass touches, so it is
// read rather than handed straight to the driver.
std::vector<uint32_t> loadSpirv(const char* name) {
    std::string path = std::string(WHIMSICAL_SHADERS) + "/" + name + ".spv";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Missing SPIR-V: " + path);
    size_t size = size_t(file.tellg());
    std::vector<uint32_t> data((size + 3) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
    return data;
}
VkClearColorValue black() {
    return {};
}
} // namespace

RenderPipeline::RenderPipeline(VulkanContext& vk, ShaderCompiler& shaders, ResourcePool& pool,
                               const RenderResources& resources)
    : vk_(vk), shaders_(shaders), pool_(pool), r_(resources) {
    for (uint32_t i = 0; i < screenSpacePasses.size(); ++i) {
        screenSpace_[i] = createCompute(loadSpirv(screenSpacePasses[i]));
        compute_[Composite + i] = &screenSpace_[i];
    }
}

RenderPipeline::~RenderPipeline() {
    vkDestroyPipeline(vk_.device, overlayProgram_, nullptr);
    vkDestroyPipeline(vk_.device, overlayIDProgram_, nullptr);
    for (const auto& program : screenSpace_)
        if (program.pipeline)
            vkDestroyPipeline(vk_.device, program.pipeline, nullptr);
    for (const auto& programs : computePrograms_)
        for (const auto& program : programs.second)
            vkDestroyPipeline(vk_.device, program.pipeline, nullptr);
    for (const auto& program : rasterPrograms_)
        vkDestroyPipeline(vk_.device, program.second, nullptr);
    for (const auto& program : entityIDPrograms_)
        vkDestroyPipeline(vk_.device, program.second, nullptr);
}

Program RenderPipeline::createCompute(const std::vector<uint32_t>& code) {
    Program program;
    program.accesses = reflect(pool_.registry(), code.data(), code.size());
    VkShaderModule module = createModule(vk_, code);
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.layout = pool_.pipelineLayout();
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = module;
    cp.stage.pName = "main";
    auto result = vkCreateComputePipelines(vk_.device, VK_NULL_HANDLE, 1, &cp, nullptr, &program.pipeline);
    vkDestroyShaderModule(vk_.device, module, nullptr);
    VK_CHECK(result);
    return program;
}

VkPipeline RenderPipeline::createRaster(const std::shared_ptr<const ShaderAsset>& shader, bool entityID, bool overlay) {
    const auto code = overlay ? loadSpirv(entityID ? "overlay_id.frag" : "overlay.frag")
                              : shaders_.compile(entityID ? "entity_id.frag" : "gbuffer.frag", {shader});
    const auto vertexCode = loadSpirv("gbuffer.vert");
    auto& accesses = overlay ? (entityID ? overlayIDAccess_ : overlayAccess_) : (entityID ? entityIDAccess_ : rasterAccess_);
    merge(accesses, reflect(pool_.registry(), vertexCode.data(), vertexCode.size()));
    merge(accesses, reflect(pool_.registry(), code.data(), code.size()));
    VkShaderModule vertex = createModule(vk_, vertexCode), fragment = createModule(vk_, code);
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
    rs.cullMode = overlay ? VK_CULL_MODE_NONE : shader->renderState.cull == SurfaceCull::Back    ? VK_CULL_MODE_BACK_BIT
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
    blend.attachmentCount = entityID || overlay ? 1 : uint32_t(attachments.size());
    blend.pAttachments = attachments.data();
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkFormat formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_FORMAT_R32_SFLOAT,          VK_FORMAT_R16G16B16A16_SFLOAT};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (entityID)
        formats[0] = VK_FORMAT_R32_UINT;
    rendering.colorAttachmentCount = entityID || overlay ? 1 : 6;
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
        SurfacePrograms linked;
        const auto& names = ShaderCompiler::surfacePasses;
        try {
            for (uint32_t i = 0; i < linked.size(); ++i)
                linked[i] = createCompute(shaders_.compile(names[i], bindings.shaders));
        } catch (...) {
            for (const auto& program : linked)
                if (program.pipeline)
                    vkDestroyPipeline(vk_.device, program.pipeline, nullptr);
            throw;
        }
        programs = computePrograms_.emplace(bindings.shaders, std::move(linked)).first;
    }
    for (uint32_t i = 0; i < programs->second.size(); ++i)
        compute_[i] = &programs->second[i];
}

void RenderPipeline::build(RenderGraph& graph, const FrameSetup& setup) {
    const auto& scene = r_.scene;
    const auto& g = r_.gbuffer;
    const auto& di = r_.di;
    const auto& shade = r_.shading;
    const Frame& frame = *setup.frame;
    graph.reset();

    if (setup.clearHistory) {
        auto resettable = pool_.registry().resettable();
        graph.add("Initialize History / Reservoirs")
            .overwrite(resettable, Access::Transfer)
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

    auto* gpuScene = setup.scene;
    const bool hasOverlays = std::any_of(frame.proxies.begin(), frame.proxies.end(), [](const RenderProxy& p) {
        return p.live && p.attributes.visible && p.attributes.overlay;
    });
    if (hasOverlays && !overlayProgram_) {
        overlayProgram_ = createRaster(nullptr, false, true);
        overlayIDProgram_ = createRaster(nullptr, true, true);
    }
    if (gpuScene->skinsDirty())
        // A refit rewrites the bottom-level structures of the meshes that moved and leaves
        // the rest standing, which is what Modify says and why the top-level build below
        // depends on it without either pass naming the other.
        graph.add("Skinned BLAS Refit")
            .read(scene.geometry(), Access::Build)
            .modify(scene.blas, Access::Build)
            .record([gpuScene](const PassContext& c) { gpuScene->recordSkinnedBlas(c.command); });

    auto* profiler = setup.profiler;
    // A refit keeps the structure it updates; a rebuild replaces it. The scene has already
    // decided which, so the graph is told rather than assuming the conservative one. The
    // bottom level comes with the top one, which is what orders this against the refit.
    graph.add("Acceleration Structures")
        .read(scene.buildInstances, Access::Build)
        .use(scene.tlas, Access::Build, gpuScene->tlasRefits() ? Usage::Modify : Usage::Overwrite)
        .record([gpuScene, profiler](const PassContext& c) { gpuScene->recordTlas(c.command, *profiler); });

    VkClearColorValue farViewZ{};
    farViewZ.float32[0] = 10000;
    const auto& programs = rasterPrograms_;
    // Attachments and the vertex and index fetch are the pass's own: they are the only
    // things a compiled shader has nothing to say about. Everything the vertex and
    // fragment stages read comes from the modules themselves.
    graph.add("GBuffer Raster")
        .color(g.albedo, black())
        .color(g.normal, black())
        .color(g.position, black())
        .color(g.motion, black())
        .color(g.viewZ, farViewZ)
        .color(g.emission, black())
        .depth(g.depth)
        .shader(rasterAccess_)
        .read(scene.vertices, Access::Vertex)
        .read(scene.indices, Access::Index)
        .record([gpuScene, &frame, &programs](const PassContext& c) {
            if (frame.materials.empty())
                return; // An empty scene still clears the attachments; it has no raster programs to bind.
            vkCmdBindDescriptorSets(c.command, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1,
                                    &c.descriptors, 0, nullptr);
            gpuScene->recordDraws(c.command, frame, programs);
        });

    if (setup.targets) {
        for (const auto& output : setup.targets->rasterOutputs()) {
            for (const auto& material : frame.materials)
                if (!entityIDPrograms_.count(material.shader))
                    entityIDPrograms_.emplace(material.shader, createRaster(material.shader, true));
            graph.add("EntityID Raster")
                .color(output.color, black())
                .depth(output.depth)
                .shader(entityIDAccess_)
                .read(scene.vertices, Access::Vertex)
                .read(scene.indices, Access::Index)
                .record([this, gpuScene, &frame](const PassContext& c) {
                    if (frame.materials.empty())
                        return;
                    vkCmdBindDescriptorSets(c.command, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1,
                                            &c.descriptors, 0, nullptr);
                    gpuScene->recordDraws(c.command, frame, entityIDPrograms_);
                });
            if (hasOverlays)
                graph.add("Overlay EntityID Raster")
                    .color(output.color)
                    .depth(output.depth)
                    .shader(overlayIDAccess_)
                    .read(scene.vertices, Access::Vertex)
                    .read(scene.indices, Access::Index)
                    .record([this, gpuScene, &frame](const PassContext& c) {
                        vkCmdBindDescriptorSets(c.command, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1,
                                                &c.descriptors, 0, nullptr);
                        gpuScene->recordDraws(c.command, frame, {}, true, overlayIDProgram_);
                    });
        }
        setup.targets->addReadbacks(graph);
    }

    // Dispatch extents cover these outputs. Reservoir passes update rotating layers,
    // so each explicitly preserves the rest of that buffer.
    graph.add("RTXDI Same Sample Gradient")
        .dispatch(*compute_[DiGradient], GradientDivisor)
        .overwrite(di.gradient, Access::Compute);
    graph.add("RTXDI Gradient Filter")
        .dispatch(*compute_[DiGradientFilter], GradientDivisor)
        .overwrite(di.filteredGradient, Access::Compute);
    graph.add("RTXDI History Confidence")
        .dispatch(*compute_[DiConfidence])
        .overwrite({di.diffuseConfidence, di.specularConfidence}, Access::Compute);
    graph.add("RTXDI Initial + Secondary GI + Specular")
        .dispatch(*compute_[Lighting])
        .modify(di.reservoirs, Access::Compute)
        .overwrite({shade.rawDiffuse, shade.rawSpecular, r_.gi.candidate}, Access::Compute);
    graph.add("RTXDI Temporal Resampling")
        .dispatch(*compute_[DiTemporal])
        .modify(di.reservoirs, Access::Compute);
    graph.add("RTXDI Spatial Resampling")
        .dispatch(*compute_[DiSpatial])
        .modify(di.reservoirs, Access::Compute);
    graph.add("ReSTIR GI Reconnection")
        .dispatch(*compute_[GiReuse])
        .overwrite(r_.gi.reservoirs, Access::Compute);
    graph.add("Visibility + Radiance Resolve")
        .dispatch(*compute_[Resolve])
        .modify({di.reservoirs, shade.rawDiffuse, shade.rawSpecular}, Access::Compute)
        .overwrite({di.confidenceHistory, di.luminance, shade.directDebug, shade.indirectDebug},
                   Access::Compute);

    auto* denoiser = setup.denoiser;
    const auto& camera = frame.camera;
    // RELAX accumulates into a texture pool of its own that the graph neither owns nor can
    // name, and that accumulation is only valid if the denoiser runs on every frame. That
    // is what sideEffect() is for: the pass survives on its own account, not by pretending
    // to write something.
    graph.add("NRD RELAX Diffuse Specular")
        .read(g.motion, Access::Compute)
        .read(g.normal, Access::Compute)
        .read(g.viewZ, Access::Compute)
        .read(di.diffuseConfidence, Access::Compute)
        .read(di.specularConfidence, Access::Compute)
        .read(shade.rawDiffuse, Access::Compute)
        .read(shade.rawSpecular, Access::Compute)
        .overwrite(shade.denoisedDiffuse, Access::Compute)
        .overwrite(shade.denoisedSpecular, Access::Compute)
        .sideEffect()
        .record([this, denoiser, &camera, profiler, index = uint32_t(setup.frameNumber), reset = setup.reset,
                 ms = setup.frameMs](const PassContext& c) {
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

    graph.add("Scene Composition + Tone Map")
        .dispatch(*compute_[Composite])
        .overwrite(shade.display, Access::Compute);

    // Helpers use the same entity/mesh transforms and depth ordering in colour and ID.
    // A fresh depth attachment keeps them in front of the scene without changing its G-buffer.
    if (hasOverlays)
        graph.add("Entity Overlay Raster")
            .color(shade.display)
            .depth(g.depth)
            .shader(overlayAccess_)
            .read(scene.vertices, Access::Vertex)
            .read(scene.indices, Access::Index)
            .record([this, gpuScene, &frame](const PassContext& c) {
                vkCmdBindDescriptorSets(c.command, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1,
                                        &c.descriptors, 0, nullptr);
                gpuScene->recordDraws(c.command, frame, {}, true, overlayProgram_);
            });

    // The readback buffers declare a Host handover, so the CPU is a consumer the graph can
    // see: it keeps these passes alive and ends the frame with the barrier that makes the
    // copy visible. Waiting on the fence alone would not.
    if (setup.audit) {
        std::vector<ImageReadback> copies;
        for (auto signal : setup.auditSignals)
            copies.push_back({signal});
        addImageReadback(graph, "Audit Readback", r_.output.audit, std::move(copies));
    }

    // The scene's temporal images are view-sized. Presentation and UI are window-sized.
    // Keeping this boundary at composition means EntityID and colour use exactly the
    // same projection, independent of where the view is placed in the application.
    auto display = shade.display, presented = r_.output.presentation;
    graph.add("Clear Presentation")
        .overwrite(presented, Access::Transfer)
        .record([presented](const PassContext& c) {
            VkClearColorValue background{{.025f, .025f, .025f, 1.f}};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            auto& image = c.image(presented);
            vkCmdClearColorImage(c.command, image.handle, image.layout, &background, 1, &range);
        });
    graph.add("Present Scene View")
        .read(display, Access::Transfer)
        .modify(presented, Access::Transfer)
        .record([display, presented, rect = setup.viewport](const PassContext& c) {
            if (!rect.width || !rect.height)
                return;
            auto& source = c.image(display);
            auto& target = c.image(presented);
            VkImageBlit region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[1] = {int32_t(source.width), int32_t(source.height), 1};
            region.dstOffsets[0] = {int32_t(rect.x), int32_t(rect.y), 0};
            region.dstOffsets[1] = {int32_t(rect.x + rect.width), int32_t(rect.y + rect.height), 1};
            vkCmdBlitImage(c.command, source.handle, source.layout, target.handle, target.layout, 1, &region,
                           VK_FILTER_NEAREST);
        });
    graph.add("RmlUi Overlay")
        .color(presented)
        .record([ui = setup.ui, uiFrame = frame.ui.get()](const PassContext& c) {
            ui->draw(c.command, c.width, c.height, uiFrame);
        });
    if (setup.capture)
        addImageReadback(graph, "Screenshot Readback", r_.output.screenshot, {{presented}});

    // The swapchain declares a Present handover, so the frame ends in PRESENT_SRC without a
    // pass whose whole job was the transition, and this blit survives because it produced
    // contents the presentation engine consumes.
    auto swapchain = r_.output.swapchain;
    graph.add("Swapchain Blit")
        .read(presented, Access::Transfer)
        .overwrite(swapchain, Access::Transfer)
        .record([presented, swapchain](const PassContext& c) {
            auto& target = c.image(swapchain);
            auto& source = c.image(presented);
            VkImageBlit region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[1] = {int32_t(source.width), int32_t(source.height), 1};
            region.dstOffsets[1] = {int32_t(target.width), int32_t(target.height), 1};
            vkCmdBlitImage(c.command, source.handle, source.layout, target.handle, target.layout, 1, &region,
                           VK_FILTER_NEAREST);
        });
}
} // namespace whimsical
