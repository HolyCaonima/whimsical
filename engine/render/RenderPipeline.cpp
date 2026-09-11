#include "RenderPipeline.h"
#include "NrdDenoiser.h"
#include "UiRenderer.h"
#include "GpuRenderTargets.h"
#include "renderCore/graph/ImageReadback.h"
#include "renderCore/vulkan/VulkanAccess.h"
#include <fstream>
#include <cstring>

namespace whimsical {
using namespace rg;

namespace {
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
rg::ClearColor black() {
    return {};
}
} // namespace

RenderPipeline::RenderPipeline(VulkanContext& vk, ShaderCompiler& shaders, rc::GraphContext& execution,
                               const RenderResources& resources)
    : vk_(vk), shaders_(shaders), execution_(execution), pool_(execution), r_(resources) {
    for (uint32_t i = 0; i < screenSpacePasses.size(); ++i) {
        compute_[Composite + i] = &execution_.compute(loadSpirv(screenSpacePasses[i]));
    }
}

RenderPipeline::~RenderPipeline() = default;

rc::Pipeline RenderPipeline::createRaster(const RasterKey& key, bool entityID) {
    const bool display = key.domain == MaterialDomain::Display;
    const auto code = shaders_.compile(entityID  ? "entity_id.frag"
                                       : display ? "display.frag"
                                                 : "gbuffer.frag",
                                       {key.shader});
    const auto vertexCode = loadSpirv("gbuffer.vert");
    auto& accesses = entityID ? entityIDAccess_ : display ? displayAccess_ : rasterAccess_;
    merge(accesses, reflect(pool_.registry(), vertexCode.data(), vertexCode.size()));
    merge(accesses, reflect(pool_.registry(), code.data(), code.size()));
    rc::GraphicsDescription desc;
    desc.layout = pool_.pipelineLayout();
    desc.bindings = {{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX},
                     {1, sizeof(uint32_t), VK_VERTEX_INPUT_RATE_INSTANCE}};
    desc.attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},  {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 32}, {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 48},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, 64},    {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 80},
        {6, 1, VK_FORMAT_R32_UINT, 0}};
    desc.cull = key.cull == SurfaceCull::Back ? VK_CULL_MODE_BACK_BIT
                : key.cull == SurfaceCull::Front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
    // Vulkan gates writes on depthTestEnable. Expose independent material switches
    // by using an always-passing test when only writing was requested.
    desc.depthTest = key.depthTest || key.depthWrite;
    desc.depthWrite = key.depthWrite;
    constexpr VkCompareOp compareOps[] = {VK_COMPARE_OP_NEVER,
                                          VK_COMPARE_OP_LESS,
                                          VK_COMPARE_OP_EQUAL,
                                          VK_COMPARE_OP_LESS_OR_EQUAL,
                                          VK_COMPARE_OP_GREATER,
                                          VK_COMPARE_OP_NOT_EQUAL,
                                          VK_COMPARE_OP_GREATER_OR_EQUAL,
                                          VK_COMPARE_OP_ALWAYS};
    desc.depthCompare = key.depthTest ? compareOps[int(key.depthCompare)] : VK_COMPARE_OP_ALWAYS;
    std::array<VkPipelineColorBlendAttachmentState, 6> attachments{};
    for (auto& a : attachments)
        a.colorWriteMask = 15;
    if (!entityID && key.blend != MaterialBlend::Opaque) {
        auto& a = attachments[0];
        a.blendEnable = VK_TRUE;
        a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        a.dstColorBlendFactor =
            key.blend == MaterialBlend::Alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
        a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        a.dstAlphaBlendFactor =
            key.blend == MaterialBlend::Alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
    }
    desc.colors = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
    if (entityID || display)
        desc.colors.resize(1);
    if (entityID)
        desc.colors[0] = VK_FORMAT_R32_UINT;
    desc.blend.assign(attachments.begin(), attachments.begin() + desc.colors.size());
    desc.depth = VK_FORMAT_D32_SFLOAT;
    return rc::graphicsProgram(vk_, desc, rc::ShaderCode(vertexCode), rc::ShaderCode(code));
}

void RenderPipeline::ensurePrograms(const std::vector<Material>& materials,
                                    const ShaderCompiler::ShaderSet& shaders) {
    for (const auto& material : materials) {
        auto key = RasterKey::from(material);
        if (!rasterPrograms_.count(key))
            rasterPrograms_.emplace(key, createRaster(key));
    }
    auto programs = computePrograms_.find(shaders);
    if (programs == computePrograms_.end()) {
        SurfacePrograms linked;
        const auto& names = ShaderCompiler::surfacePasses;
        for (uint32_t i = 0; i < linked.size(); ++i)
            linked[i] = &execution_.compute(shaders_.compile(names[i], shaders));
        programs = computePrograms_.emplace(shaders, std::move(linked)).first;
    }
    for (uint32_t i = 0; i < programs->second.size(); ++i)
        compute_[i] = programs->second[i];
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
                    if (c.declaration(ref.id).kind == Kind::Image)
                        vkCmdClearColorImage(c.command, c.image(ref).handle, c.image(ref).layout, &clear, 1,
                                             &range);
                    else
                        vkCmdFillBuffer(c.command, c.buffer(ref).handle, 0, VK_WHOLE_SIZE, 0);
            });
    }

    auto* gpuScene = setup.scene;
    // Scheduling owns visibility, material selection and layer membership. Both
    // colour and picking are built together, so their coverage/order cannot drift.
    struct DrawGroup {
        std::vector<RasterDraw> items;
        std::vector<RasterBatch> color, entityID;
    };
    std::map<std::pair<MaterialDomain, uint32_t>, DrawGroup> groups;
    const bool picking = setup.targets && !setup.targets->rasterOutputs().empty();
    const auto eye = frame.camera.eye();
    gpuScene->beginDraws();
    // A component remains one proxy and one sort item, even with millions of
    // authored instances. Its internal order is never changed by scheduling.
    for (uint32_t slot = 0; slot < frame.proxies.size(); ++slot) {
        const auto& proxy = frame.proxies[slot];
        if (!proxy.live || !proxy.attributes.visible)
            continue;
        const auto& material = frame.materials[proxy.attributes.material];
        auto& group = groups[{material.renderState.domain, material.renderState.layer}];
        const auto key = RasterKey::from(material);
        // Opaque first, then blended origins back-to-front. The raster backend only
        // interprets the resulting uint32 order, not the scheduling policy.
        uint32_t sortKey = 0;
        if (material.renderState.blend != MaterialBlend::Opaque) {
            const auto delta = proxy.transform.position - eye;
            const float distanceSquared = glm::dot(delta, delta);
            std::memcpy(&sortKey, &distanceSquared, sizeof(sortKey));
            sortKey = ~sortKey; // Nonnegative IEEE floats sort by their unsigned bits.
        }
        group.items.push_back({slot, sortKey, rasterPrograms_.at(key)->pipeline});
        if (picking && !entityIDPrograms_.count(key))
            entityIDPrograms_.emplace(key, createRaster(key, true));
    }
    for (auto& entry : groups) {
        auto& group = entry.second;
        std::vector<RasterDraw> ids;
        if (picking)
            for (const auto& draw : group.items) {
                const auto& material = frame.materials[frame.proxies[draw.slot].attributes.material];
                ids.push_back({draw.slot, draw.sortKey, entityIDPrograms_.at(RasterKey::from(material))->pipeline});
            }
        group.color = gpuScene->prepareDraws(std::move(group.items));
        group.entityID = gpuScene->prepareDraws(std::move(ids));
    }
    gpuScene->uploadDraws();
    auto record = [gpuScene](std::vector<RasterBatch> draws) {
        return [gpuScene, draws = std::move(draws)](const PassContext& c) {
            if (draws.empty())
                return;
            vkCmdBindDescriptorSets(c.command, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1,
                                    &c.descriptors, 0, nullptr);
            gpuScene->recordDraws(c.command, draws);
        };
    };
    auto& surfaces = groups[{MaterialDomain::Surface, 0}];
    if (gpuScene->skinsDirty())
        // A refit rewrites the bottom-level structures of the meshes that moved and leaves
        // the rest standing, which is what Modify says and why the top-level build below
        // depends on it without either pass naming the other.
        graph.add("Skinned BLAS Refit")
            .read(scene.geometry(), Access::Build)
            .modify(scene.blas, Access::Build)
            .record([gpuScene](const PassContext& c) { gpuScene->recordSkinnedBlas(c.command); });

    // A refit keeps the structure it updates; a rebuild replaces it. The scene has already
    // decided which, so the graph is told rather than assuming the conservative one. The
    // bottom level comes with the top one, which is what orders this against the refit.
    graph.add("Acceleration Structures")
        .read(scene.buildInstances, Access::Build)
        .use(scene.tlas, Access::Build, gpuScene->tlasRefits() ? Usage::Modify : Usage::Overwrite)
        .record([gpuScene](const PassContext& c) { gpuScene->recordTlas(c.command, c.profiler()); });

    rg::ClearColor farViewZ{10000, 0, 0, 0};
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
        .read(scene.drawInstances, Access::Vertex)
        .read(scene.indices, Access::Index)
        .record(record(surfaces.color));

    if (setup.targets) {
        for (const auto& output : setup.targets->rasterOutputs()) {
            graph.add("EntityID Raster")
                .color(output.color, black())
                .depth(output.depth)
                .shader(entityIDAccess_)
                .read(scene.vertices, Access::Vertex)
                .read(scene.drawInstances, Access::Vertex)
                .read(scene.indices, Access::Index)
                .record(record(surfaces.entityID));
            for (const auto& entry : groups) {
                if (entry.first.first != MaterialDomain::Display)
                    continue;
                auto layer = entry.first.second;
                graph.add("Display EntityID Layer " + std::to_string(layer))
                    .color(output.color)
                    .depth(output.depth, layer ? std::optional<float>(1.f) : std::nullopt)
                    .shader(entityIDAccess_)
                    .read(scene.vertices, Access::Vertex)
                    .read(scene.drawInstances, Access::Vertex)
                    .read(scene.indices, Access::Index)
                    .record(record(entry.second.entityID));
            }
        }
        setup.targets->addReadbacks(graph);
    }

    // Dispatch extents cover these outputs. Reservoir passes update rotating layers,
    // so each explicitly preserves the rest of that buffer.
    graph.add("RTXDI Same Sample Gradient")
        .dispatch(*compute_[DiGradient], di.gradient)
        .overwrite(di.gradient, Access::Compute);
    graph.add("RTXDI Gradient Filter")
        .dispatch(*compute_[DiGradientFilter], di.filteredGradient)
        .overwrite(di.filteredGradient, Access::Compute);
    graph.add("RTXDI History Confidence")
        .dispatch(*compute_[DiConfidence], di.diffuseConfidence)
        .overwrite({di.diffuseConfidence, di.specularConfidence}, Access::Compute);
    graph.add("RTXDI Initial + Secondary GI + Specular")
        .dispatch(*compute_[Lighting], shade.rawDiffuse)
        .modify(di.reservoirs, Access::Compute)
        .overwrite({shade.rawDiffuse, shade.rawSpecular, r_.gi.candidate}, Access::Compute);
    graph.add("RTXDI Temporal Resampling")
        .dispatch(*compute_[DiTemporal], g.depth)
        .modify(di.reservoirs, Access::Compute);
    graph.add("RTXDI Spatial Resampling")
        .dispatch(*compute_[DiSpatial], g.depth)
        .modify(di.reservoirs, Access::Compute);
    graph.add("ReSTIR GI Reconnection")
        .dispatch(*compute_[GiReuse], g.depth)
        .overwrite(r_.gi.reservoirs, Access::Compute);
    graph.add("Visibility + Radiance Resolve")
        .dispatch(*compute_[Resolve], shade.rawDiffuse)
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
        .record([this, denoiser, &camera, index = uint32_t(setup.frameNumber), reset = setup.reset,
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
            denoiser->dispatch(c.command, resources, camera, index, reset, ms, c.profiler());
        });

    graph.add("Scene Composition + Tone Map")
        .dispatch(*compute_[Composite], shade.display)
        .overwrite(shade.display, Access::Compute);

    for (const auto& entry : groups) {
        if (entry.first.first != MaterialDomain::Display)
            continue;
        auto layer = entry.first.second;
        graph.add("Display Color Layer " + std::to_string(layer))
            .color(shade.display)
            .depth(layer ? shade.layerDepth : g.depth, layer ? std::optional<float>(1.f) : std::nullopt)
            .shader(displayAccess_)
            .read(scene.vertices, Access::Vertex)
            .read(scene.drawInstances, Access::Vertex)
            .read(scene.indices, Access::Index)
            .record(record(entry.second.color));
    }

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
