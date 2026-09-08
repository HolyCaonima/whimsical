#include "Renderer.h"
#include "GpuScene.h"
#include "NrdDenoiser.h"
#include "RenderAuditWorker.h"
#include "RenderPipeline.h"
#include "RenderResources.h"
#include "UiRenderer.h"
#include "VulkanContext.h"
#include "graph/RenderGraph.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace afterlight {
namespace {
static_assert(sizeof(GpuGlobals) == 368, "Globals ABI mismatch");
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
    double statisticsGpuSum = 0, statisticsCpuSum = 0;
    uint64_t lastCpuProfileRequest = 0;
    std::optional<CpuProfile> cpuProfileResult;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<Image> swapImages;
    std::vector<VkSemaphore> finished;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    std::unique_ptr<GpuProfiler> profiler;
    uint64_t lastProfileRequest = 0;
    ShaderCompiler shaderCompiler;
    MaterialBindings materialBindings;

    RenderResources resources;
    std::unique_ptr<rg::ResourcePool> pool;
    std::unique_ptr<rg::RenderGraph> graph;
    std::unique_ptr<GpuScene> scene;
    std::unique_ptr<RenderPipeline> pipeline;
    std::unique_ptr<NrdDenoiser> denoiser;
    std::unique_ptr<UiRenderer> uiRenderer;
    std::unique_ptr<RenderAuditWorker> audit;
    rg::ResourceList auditSignals;
    Buffer neighbourOffsets;
    bool auditReadbackPending = false;
    uint32_t auditSamples = 0;

    FrameRef previous;
    bool historyValid = false;
    bool resizePending = false;
    std::chrono::steady_clock::time_point previousRenderTime = std::chrono::steady_clock::now();

    Impl(HWND hwnd, const RenderOptions& opts)
        : options(opts), window(hwnd),
          resources(opts.capture, opts.audit.empty() ? 0 : RenderAudit::signalCount) {}

    ~Impl() {
        if (!vk.device)
            return;
        vkDeviceWaitIdle(vk.device);
        profiler.reset();
        pipeline.reset();
        denoiser.reset();
        uiRenderer.reset();
        scene.reset();
        pool.reset();
        vk.destroy(neighbourOffsets);
        for (auto s : finished)
            vkDestroySemaphore(vk.device, s, nullptr);
        if (swapchain)
            vkDestroySwapchainKHR(vk.device, swapchain, nullptr);
        if (acquired)
            vkDestroySemaphore(vk.device, acquired, nullptr);
        if (fence)
            vkDestroyFence(vk.device, fence, nullptr);
    }

    void initialize() {
        vk.initialize(window, options.validation);
        pool = std::make_unique<rg::ResourcePool>(vk, resources.registry);
        graph = std::make_unique<rg::RenderGraph>(*pool);
        scene = std::make_unique<GpuScene>(vk, options);
        scene->bind(*pool, resources.scene);
        // RTXDI's spatial neighbour offsets are a fixed Vandercorput-style disc, written
        // once and never touched again, so the scene owns them rather than the graph.
        neighbourOffsets =
            vk.buffer(1024 * sizeof(vec2), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
        auto* offsets = static_cast<vec2*>(neighbourOffsets.mapped);
        for (uint32_t i = 0; i < 1024; ++i) {
            float radius = std::sqrt((i + .5f) / 1024.f), angle = i * 2.39996323f;
            offsets[i] = radius * vec2(std::cos(angle), std::sin(angle));
        }
        pool->importBuffer(resources.di.neighbours, neighbourOffsets);
        pool->importBuffer(resources.di.lightSamples, scene->lightDistribution);
        pipeline = std::make_unique<RenderPipeline>(vk, shaderCompiler, *pool, resources);
        denoiser = std::make_unique<NrdDenoiser>(vk);
        uiRenderer = std::make_unique<UiRenderer>(vk);
        for (const char* name : RenderAudit::names)
            auditSignals.push_back(auditSignal(name));
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
        profiler = std::make_unique<GpuProfiler>(vk);
    }

    // The audit names signals, not bindings; the graph resolves them to resources.
    rg::ResourceId auditSignal(const std::string& name) const {
        const auto& s = resources.shading;
        if (name == "direct")
            return s.directDebug;
        if (name == "albedo")
            return resources.gbuffer.albedo;
        if (name == "display")
            return s.display;
        if (name == "raw-diffuse")
            return s.rawDiffuse;
        if (name == "raw-specular")
            return s.rawSpecular;
        if (name == "denoised-diffuse")
            return s.denoisedDiffuse;
        if (name == "denoised-specular")
            return s.denoisedSpecular;
        throw std::runtime_error("Audit signal has no render graph resource: " + name);
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

    // Returns false when the surface currently has no area, which happens when the window
    // is minimised between the simulation sampling its size and this query running on the
    // render thread. Sizing resources to that would ask Vulkan for zero-extent images.
    bool resize(uint32_t requestedWidth, uint32_t requestedHeight) {
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
        if (!extent.width || !extent.height) {
            resizePending = true; // Retry once the window has area again.
            return false;
        }
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
        swapImages.reserve(count);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto handle : handles) {
            Image i;
            i.generation = nextResourceGeneration();
            i.handle = handle;
            i.format = format.format;
            i.width = width;
            i.height = height;
            swapImages.push_back(i);
            VkSemaphore semaphore;
            VK_CHECK(vkCreateSemaphore(vk.device, &semaphoreInfo, nullptr, &semaphore));
            finished.push_back(semaphore);
        }
        // A resize starts a new image-sized audit, as with the temporal histories.
        if (audit)
            audit->finish();
        audit.reset();
        if (!options.audit.empty())
            audit = std::make_unique<RenderAuditWorker>(width, height);
        denoiser->resize(width, height);
        historyValid = false;
        resizePending = false;
        statistics = {};
        statistics.present = presentName(presentMode);
        statistics.vsync = presentMode == VK_PRESENT_MODE_FIFO_KHR;
        statisticsStart = {};
        statisticsIntervals = 0;
        statisticsGpuSum = 0;
        statisticsCpuSum = 0;
        return true;
    }

    void saveCapture() {
        std::filesystem::path dir = std::filesystem::path(AFTERLIGHT_ROOT) / "captures";
        if (!options.audit.empty())
            dir /= options.audit;
        std::filesystem::create_directories(dir);
        auto* rgba = static_cast<uint8_t*>(pool->buffer(resources.output.screenshot).mapped);
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
               << ",\n  \"cpuRenderAverageMs\": " << statistics.cpuMs
               << ",\n  \"simulationHz\": 60,\n  \"presentMode\": \"" << presentName(presentMode) << "\""
               << ",\n  \"validationActive\": " << (vk.validationActive ? "true" : "false")
               << ",\n  \"validationErrors\": " << vk.validationErrors.load()
               << ",\n  \"auditEnabled\": " << (audit ? "true" : "false")
               << ",\n  \"auditSamples\": " << auditSamples << ",\n  \"auditCpuAverageMs\": "
               << (auditSamples ? audit->cpuMilliseconds() / auditSamples : 0)
               << ",\n  \"exposure\": " << previous->exposure << ", \"debugView\": " << previous->debugView
               << ", \"hudEnabled\": " << (previous->hudEnabled ? "true" : "false")
               << ", \"consoleOpen\": " << (previous->console.open ? "true" : "false")
               << ",\n  \"staticMeshInstances\": " << scene->staticMeshInstances()
               << ", \"geometryBuilds\": " << scene->geometryBuilds
               << ", \"geometryReleases\": " << scene->geometryReleases
               << ", \"geometryBufferGrowths\": " << scene->geometryBufferGrowths
               << ", \"staticMeshAssets\": " << scene->staticMeshAssets()
               << ", \"textureAssets\": " << scene->textureAssets()
               << ", \"shaderAssets\": " << materialBindings.shaders.size()
               << ", \"shaderCompilations\": " << shaderCompiler.compilationCount()
               << ", \"rasterPrograms\": " << pipeline->rasterProgramCount()
               << ",\n  \"sceneSlots\": " << scene->statistics.slots
               << ", \"sceneSlotWrites\": " << scene->writes
               << ", \"sceneSlotWritesIfRebuilt\": " << scene->slotFrames
               << ",\n  \"sceneResyncs\": " << scene->resyncs
               << ", \"tlasRebuilds\": " << scene->tlasRebuilds
               << ",\n  \"graphPasses\": " << graph->livePasses() << ", \"graphBarriers\": "
               << graph->barrierCount() << ", \"graphAliasedResources\": " << graph->aliasedResources()
               << ",\n  \"graphResourceBytes\": " << pool->ownedBytes()
               << ", \"graphDeclaredBytes\": " << pool->declaredBytes()
               << ", \"graphDescriptorWrites\": " << pool->descriptorWrites()
               << ",\n  \"meanRgb\": " << double(sum) / (double(width) * height * 3) << ",\n  \"redRange\": ["
               << int(minimum) << "," << int(maximum) << "]\n}\n";
        std::cout << "Capture: " << (dir / "frame.bmp").string() << " | " << statistics.fps << " FPS | Frame "
                  << statistics.frameMs << " ms | CPU (Render) " << statistics.cpuMs << " ms | GPU "
                  << statistics.gpuMs << " ms\n";
    }

    void updateStatistics(double cpuMs) {
        const auto now = std::chrono::steady_clock::now();
        if (statisticsStart == std::chrono::steady_clock::time_point{}) {
            statisticsStart = now;
            return;
        }
        ++statisticsIntervals;
        statisticsGpuSum += gpuMs;
        statisticsCpuSum += cpuMs;
        const double elapsed = std::chrono::duration<double>(now - statisticsStart).count();
        if (elapsed >= .5) {
            // Count actual frame intervals, including snapshot, GPU and present waits.
            // GPU duration alone is not complete frame time and cannot be inverted for FPS.
            statistics.fps = statisticsIntervals / elapsed;
            statistics.frameMs = elapsed * 1000 / statisticsIntervals;
            statistics.gpuMs = statisticsGpuSum / statisticsIntervals;
            statistics.cpuMs = statisticsCpuSum / statisticsIntervals;
            statisticsStart = now;
            statisticsIntervals = 0;
            statisticsGpuSum = 0;
            statisticsCpuSum = 0;
        }
    }

    // Called only after the existing frame fence has completed, before this readback
    // buffer can be reused or resized. The worker never retains a GPU mapped pointer.
    void collectAuditReadback() {
        if (!auditReadbackPending)
            return;
        audit->submit(static_cast<const uint16_t*>(pool->buffer(resources.output.audit).mapped));
        auditReadbackPending = false;
    }

    bool render(const FrameRef& sourceFrame) {
        const bool captureCpu =
            sourceFrame->cpuProfile && sourceFrame->cpuProfile->request > lastCpuProfileRequest;
        CpuProfiler cpuProfiler(captureCpu, "Render", "Render Frame");
        FrameRef frameRef = sourceFrame;
        if (options.auditMotion || options.auditOccluder >= 0 || options.auditLight >= 0) {
            Frame diagnostic = *sourceFrame;
            if (options.auditMotion)
                diagnostic.camera.yaw += .04f * std::sin(float(frameNumber) * .017f);
            if (options.auditOccluder >= 0) {
                diagnostic.proxies.at(size_t(options.auditOccluder)).transform.position.x +=
                    frameNumber >= 64 ? 2.f : 0.f;
                diagnostic.forceFullUpload = true;
            }
            if (options.auditLight >= 0 && frameNumber >= 64) {
                auto& light = diagnostic.lights.at(size_t(options.auditLight));
                light.positionRadius.x += 2.f;
                light.colorIntensity.w *= .25f;
            }
            frameRef = std::make_shared<const Frame>(std::move(diagnostic));
        }
        const Frame& frame = *frameRef;
        if (!frame.input.width || !frame.input.height)
            return true;
        if (frame.proxies.size() > MaxInstances || frame.materials.size() > MaxMaterials ||
            frame.lights.size() > MaxLights)
            throw std::runtime_error("Scene capacity exceeded");
        {
            CpuScope scope("Wait / Previous GPU Fence");
            VK_CHECK(vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX));
        }
        collectAuditReadback();
        {
            CpuScope scope("Resolve GPU Timestamps");
            profiler->resolve();
        }
        gpuMs = profiler->frameMs();
        if (width != frame.input.width || height != frame.input.height || resizePending) {
            CpuScope scope("Swapchain Resize / Resource Rebuild");
            if (!resize(frame.input.width, frame.input.height))
                return true;
        }
        uint32_t swapIndex;
        VkResult acquire;
        {
            CpuScope scope("Wait / Acquire Swapchain Image");
            acquire =
                vkAcquireNextImageKHR(vk.device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &swapIndex);
        }
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            resizePending = true;
            return true;
        }
        if (acquire != VK_SUBOPTIMAL_KHR)
            VK_CHECK(acquire);
        else
            resizePending = true;
        const auto cpuStart = std::chrono::steady_clock::now();
        CpuScope prepareScope("Prepare / Record / Submit");
        float frameMs = std::clamp(
            float(std::chrono::duration<double, std::milli>(cpuStart - previousRenderTime).count()), 1.f,
            100.f);
        previousRenderTime = cpuStart;
        {
            CpuScope scope("CPU Skinning / Mesh Bindings");
            scene->updateSkins(frame);
        }
        CpuScope uploadScope("Scene / Globals / Material Upload");
        bool reset = !historyValid || frame.resetHistory ||
                     glm::distance(frame.camera.eye(), previous->camera.eye()) > 4.f;
        bool lightsChanged = !historyValid, materialsChanged = !historyValid;
        if (historyValid) {
            lightsChanged = frame.lights.size() != previous->lights.size() ||
                            std::memcmp(frame.lights.data(), previous->lights.data(),
                                        frame.lights.size() * sizeof(Light)) != 0;
            materialsChanged = !(frame.materials == previous->materials);
            // Stable light slots permit parameter animation; topology changes restart history.
            reset = reset || frame.lightEntities != previous->lightEntities ||
                    frame.lights.size() != previous->lights.size() || materialsChanged;
        }
        if (materialsChanged) {
            materialBindings = MaterialBindings::build(frame.materials);
            pipeline->ensurePrograms(materialBindings);
            scene->updateMaterials(materialBindings);
        }
        if (scene->takeHistoryInvalidation())
            historyValid = false;
        const bool clearHistory = !historyValid;
        reset = reset || clearHistory;
        GpuGlobals data{};
        data.view = frame.camera.view();
        data.vp = frame.camera.projection(float(width) / height) * data.view;
        data.previousVp =
            reset ? data.vp : previous->camera.projection(float(width) / height) * previous->camera.view();
        data.inverseVp = glm::inverse(data.vp);
        data.eyeTime = vec4(frame.camera.eye(), float(frame.time));
        data.previousEye = vec4(reset ? frame.camera.eye() : previous->camera.eye(), 0);
        data.resolution = {float(width), float(height), float(frame.debugView), float(frame.hovered)};
        data.player = vec4(frame.player, float(frame.selected));
        data.destination = vec4(frame.destination, frame.hasDestination ? 1.f : 0.f);
        // The last component tells the DI bridge which half of the reservoir arrays is
        // playing which role this frame; it replaces the end-of-frame history copy.
        data.renderSettings = {frame.exposure, frame.diHistoryConfidence ? 1.f : 0.f,
                               lightsChanged ? 1.f : 0.f,
                               float((frameNumber & 1) ? DiLayerRotation : 0)};
        data.counts = {uint32_t(frame.proxies.size()), uint32_t(frame.lights.size()), uint32_t(frameNumber),
                       reset ? 0u : 1u};
        scene->writeGlobals(&data, sizeof(data));
        scene->apply(frame, reset);
        // Materials and lights are compared against the previous snapshot anyway, to
        // decide whether temporal history survives; the same answer decides whether they
        // are worth uploading again.
        if (lightsChanged)
            scene->writeLights(frame.lights);
        scene->writeLightDistribution(frame.lights, reset ? frame.lights : previous->lights);
        uploadScope.finish();
        {
            CpuScope scope("UI / Prepare Resources");
            uiRenderer->prepare(frame.ui.get());
        }

        const bool auditFrame = audit && frameNumber >= 64;
        const bool last = options.maxFrames && frameNumber + 1 >= options.maxFrames;
        const bool captureFrame = options.capture && last;
        pool->importImage(resources.output.swapchain, swapImages[swapIndex]);

        FrameSetup setup;
        setup.frame = &frame;
        setup.scene = scene.get();
        setup.denoiser = denoiser.get();
        setup.ui = uiRenderer.get();
        setup.profiler = profiler.get();
        setup.frameNumber = frameNumber;
        setup.frameMs = frameMs;
        setup.reset = reset;
        setup.clearHistory = clearHistory;
        setup.audit = auditFrame;
        setup.capture = captureFrame;
        setup.auditSignals = auditSignals;
        {
            CpuScope scope("Graph / Declare");
            pipeline->build(*graph, setup);
        }
        graph->compile(width, height);

        CpuScope recordScope("Record GPU Commands");
        VK_CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command, &begin));
        const auto request = frame.gpuProfileRequest > lastProfileRequest ? frame.gpuProfileRequest : 0;
        profiler->beginFrame(command, request, frameNumber + 1, width, height);
        if (request)
            lastProfileRequest = request;
        graph->execute(command, *profiler);
        profiler->endFrame(command);
        VK_CHECK(vkEndCommandBuffer(command));
        recordScope.finish();

        CpuScope submitScope("Queue Submit");
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
        submitScope.finish();
        prepareScope.finish();
        // Measure CPU preparation/recording/submission directly, never Frame minus GPU.
        // Keep swapchain pacing and the next frame's GPU fence outside this interval.
        const double cpuMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &finished[swapIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &swapIndex;
        VkResult result;
        {
            CpuScope scope("Wait / Present");
            result = vkQueuePresentKHR(vk.queue, &present);
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            resizePending = true;
        else
            VK_CHECK(result);
        previous = frameRef;
        historyValid = true;
        frameNumber++;
        // Both halves of every history resource change role now, which is the whole of
        // what used to be the end-of-frame copy pass.
        pool->flip();
        auditReadbackPending = auditFrame;
        updateStatistics(cpuMs);
        if (last) {
            CpuScope scope("Final Frame / Wait and Capture");
            VK_CHECK(vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX));
            collectAuditReadback();
            if (audit) {
                CpuScope auditScope("Audit / Drain Worker and Save");
                const auto output = std::filesystem::path(AFTERLIGHT_ROOT) / "captures" / options.audit;
                auditSamples = audit->finish(output).samples;
            }
            profiler->resolve();
            gpuMs = profiler->frameMs();
            if (options.capture)
                saveCapture();
        }
        if (captureCpu) {
            auto report = *frame.cpuProfile;
            report.frame = frameNumber;
            report.threads.push_back(cpuProfiler.finish());
            lastCpuProfileRequest = report.request;
            cpuProfileResult = std::move(report);
        }
        return !last;
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
SceneUpdateStatistics Renderer::sceneStatistics() const {
    return impl_->scene->statistics;
}
std::optional<GpuProfile> Renderer::takeGpuProfile() {
    return impl_->profiler->takeResult();
}
std::optional<CpuProfile> Renderer::takeCpuProfile() {
    auto result = std::move(impl_->cpuProfileResult);
    impl_->cpuProfileResult.reset();
    return result;
}
uint32_t Renderer::errors() const {
    return impl_->vk.validationErrors.load();
}
} // namespace afterlight
