#include "Renderer.h"
#include "MaterialBindings.h"
#include "VulkanContext.h"
#include "NrdDenoiser.h"
#include "RenderGraph.h"
#include "Geometry.h"
#include "RangeAllocator.h"
#include <set>
#include "animation/SkinnedMesh.h"
#include "assets/StaticMesh.h"
#include <map>
#include "UiRenderer.h"
#include "RenderAuditWorker.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <Rtxdi/RtxdiParameters.h>
namespace afterlight {
namespace {
constexpr uint32_t MaxInstances = 1024, MaxLights = 256, MaxMaterials = 256, MaxTextures = 64;
static_assert(sizeof(RTXDI_PackedDIReservoir) == 24, "RTXDI packed reservoir ABI changed");
constexpr VkImageUsageFlags ColorUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
struct alignas(16) Globals {
    mat4 vp, previousVp, view, inverseVp;
    vec4 eyeTime, resolution, player, destination, renderSettings, previousEye;
    glm::uvec4 counts;
};
struct alignas(16) GpuInstance {
    mat4 model, previousModel;
    glm::uvec4 info;
};
static_assert(sizeof(Globals) == 368 && sizeof(GpuInstance) == 144 && sizeof(GpuMaterial) == 176 &&
                  sizeof(GpuVertex) == 96,
              "GPU layout mismatch");
struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer storage;
    VkDeviceAddress address = 0;
};
VkShaderModule createShaderModule(VulkanContext& vk, const std::vector<uint32_t>& data) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = data.size() * sizeof(uint32_t);
    info.pCode = data.data();
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
    double statisticsCpuSum = 0;
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
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    ShaderCompiler shaderCompiler;
    MaterialBindings materialBindings;
    std::map<std::shared_ptr<const ShaderAsset>, VkPipeline> rasterPrograms;
    std::map<ShaderCompiler::ShaderSet, std::array<VkPipeline, ShaderCompiler::surfacePasses.size()>>
        computePrograms;
    enum Pass {
        Lighting,
        GiReuse,
        DiTemporal,
        DiSpatial,
        Resolve,
        DiGradient,
        Composite,
        DiConfidence,
        DiGradientFilter,
        PassCount
    };
    std::array<VkPipeline, PassCount> compute{};
    Buffer globals, instanceData, materialData, lightData, vertexData, indexData, tlasInstances, tlasScratch,
        readback, auditReadback;
    std::unique_ptr<RenderAuditWorker> audit;
    bool auditReadbackPending = false;
    uint32_t auditSamples = 0;
    std::array<Buffer, 6> reservoirs;
    std::vector<AccelerationStructure> blas;
    std::vector<Buffer> blasScratch;
    AccelerationStructure tlas;
    uint32_t tlasCount = 0;
    bool tlasBuilt = false;
    std::vector<MeshRange> meshes;
    struct MeshAllocation {
        uint32_t firstVertex = 0, vertexCount = 0;
        bool dynamic = false;
    };
    std::vector<MeshAllocation> meshAllocations;
    std::vector<uint32_t> freeMeshes, geometryIndices, dirtySkinMeshes;
    RangeAllocator vertexRanges, indexRanges;
    std::map<std::shared_ptr<const StaticMesh>, uint32_t> staticAssets;
    struct SkinDraw {
        uint32_t entity = 0, meshIndex = 0;
        std::shared_ptr<const SkinnedMesh> mesh;
        std::vector<mat4> palette;
        bool settling = false;
    };
    std::map<uint32_t, SkinDraw> skinDraws;
    std::map<uint32_t, uint32_t> skinMeshes, staticSlots;
    uint64_t geometryTopology = UINT64_MAX;
    uint64_t geometryBuilds = 0, geometryReleases = 0, geometryBufferGrowths = 0;
    std::vector<Image> materialTextures;
    VkSampler materialSampler = VK_NULL_HANDLE;
    std::vector<std::shared_ptr<const TextureAsset>> textureBindings;
    std::vector<GpuVertex> geometryVertices;
    uint32_t vertexCount = 0;
    // Binding number -> image; unused bindings intentionally remain empty.
    std::array<Image, 39> images;
    Image depth, captureImage;
    std::unique_ptr<NrdDenoiser> denoiser;
    std::unique_ptr<UiRenderer> uiRenderer;
    FrameRef previous;
    // Persistent mirror of the render scene. The renderer keeps its own copy of every
    // slot's model matrix because the GPU-side instance buffer lives in write-combined
    // memory, where reading back what was written costs far more than storing it twice.
    std::vector<mat4> shadowModel;
    std::vector<uint64_t> motionFrame; // Stamp of the update in which a slot last moved.
    std::vector<uint32_t> movedThisFrame, movedLastFrame;
    uint64_t sceneStamp = 0;
    uint64_t mirrorRevision = 0, mirrorTopology = 0;
    uint32_t tlasCapacity = 0;
    uint32_t mirrorCapacity = 0;
    bool mirrorValid = false;
    bool descriptorsDirty = true;
    SceneUpdateStatistics sceneStatistics;
    // Totals over the whole run, so the incremental path can be compared against what a
    // full rebuild of every slot on every frame would have cost.
    uint64_t sceneWrites = 0, sceneResyncs = 0, sceneTlasRebuilds = 0, sceneSlotFrames = 0;
    bool historyValid = false;
    bool resizePending = false;
    std::chrono::steady_clock::time_point previousRenderTime = std::chrono::steady_clock::now();
    Impl(HWND hwnd, const RenderOptions& opts) : options(opts), window(hwnd) {}
    ~Impl() {
        if (!vk.device)
            return;
        vkDeviceWaitIdle(vk.device);
        profiler.reset();
        denoiser.reset();
        uiRenderer.reset();
        for (auto& b : reservoirs)
            vk.destroy(b);
        for (auto& i : images)
            vk.destroy(i);
        vk.destroy(depth);
        vk.destroy(captureImage);
        for (auto& texture : materialTextures)
            vk.destroy(texture);
        if (materialSampler)
            vkDestroySampler(vk.device, materialSampler, nullptr);
        for (auto b : {&globals, &instanceData, &materialData, &lightData, &vertexData, &indexData,
                       &tlasInstances, &tlasScratch, &readback, &auditReadback})
            vk.destroy(*b);
        for (auto& a : blas)
            destroyAS(a);
        for (auto& b : blasScratch)
            vk.destroy(b);
        destroyAS(tlas);
        for (auto s : finished)
            vkDestroySemaphore(vk.device, s, nullptr);
        if (swapchain)
            vkDestroySwapchainKHR(vk.device, swapchain, nullptr);
        if (acquired)
            vkDestroySemaphore(vk.device, acquired, nullptr);
        if (fence)
            vkDestroyFence(vk.device, fence, nullptr);
        for (auto i : {Composite, DiConfidence, DiGradientFilter})
            if (compute[i])
                vkDestroyPipeline(vk.device, compute[i], nullptr);
        for (const auto& programs : computePrograms)
            for (auto p : programs.second)
                vkDestroyPipeline(vk.device, p, nullptr);
        for (const auto& program : rasterPrograms)
            vkDestroyPipeline(vk.device, program.second, nullptr);
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
        globals = vk.buffer(sizeof(Globals), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, BufferMemory::Upload);
        instanceData = vk.buffer(sizeof(GpuInstance) * MaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 BufferMemory::Upload);
        materialData = vk.buffer(sizeof(GpuMaterial) * MaxMaterials, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 BufferMemory::Upload);
        lightData =
            vk.buffer(sizeof(Light) * MaxLights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
        tlasInstances = vk.buffer(sizeof(VkAccelerationStructureInstanceKHR) * MaxInstances,
                                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  BufferMemory::Upload);
        initializeGeometry();
        updateTextures({});
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
        profiler = std::make_unique<GpuProfiler>(vk);
    }
    uint32_t meshFor(uint32_t slot, const RenderProxy& p) const {
        auto mesh = staticSlots.find(slot);
        if (mesh != staticSlots.end())
            return mesh->second;
        auto found = skinMeshes.find(slot);
        return found == skinMeshes.end() ? uint32_t(p.attributes.shape) : found->second;
    }
    void buildMeshAS(VkCommandBuffer c, uint32_t m, bool update) {
        VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.flags = 0; // Candidate acceptance uses the Shader surface/cull contract.
        geom.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        auto& tri = geom.geometry.triangles;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = vertexData.address;
        tri.vertexStride = sizeof(GpuVertex);
        tri.maxVertex = meshAllocations[m].firstVertex + meshAllocations[m].vertexCount - 1;
        tri.indexType = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress = indexData.address + meshes[m].firstIndex * sizeof(uint32_t);
        VkAccelerationStructureBuildGeometryInfoKHR build{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        if (meshAllocations[m].dynamic)
            build.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        build.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                            : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geom;
        uint32_t primitives = meshes[m].indexCount / 3;
        if (!update) {
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(
                vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes);
            ++geometryBuilds;
            blas[m] = createAS(build.type, sizes.accelerationStructureSize);
            blasScratch[m] =
                vk.buffer(std::max(sizes.buildScratchSize, sizes.updateScratchSize) +
                              vk.asProperties.minAccelerationStructureScratchOffsetAlignment,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        }
        build.srcAccelerationStructure = update ? blas[m].handle : VK_NULL_HANDLE;
        build.dstAccelerationStructure = blas[m].handle;
        build.scratchData.deviceAddress = alignedScratch(blasScratch[m]);
        VkAccelerationStructureBuildRangeInfoKHR range{primitives, 0, 0, 0};
        const auto* ptr = &range;
        vkCmdBuildAccelerationStructuresKHR(c, 1, &build, &ptr);
    }

    void uploadGeometry(const std::vector<uint32_t>& added) {
        auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        auto upload = [&](Buffer& buffer, const void* data, size_t bytes, VkBufferUsageFlags extra) {
            if (buffer.size >= bytes)
                return false;
            auto capacity = std::max<VkDeviceSize>(bytes, std::max<VkDeviceSize>(buffer.size * 2, 4096));
            vk.destroy(buffer);
            buffer = vk.buffer(capacity, usage | extra, BufferMemory::Upload);
            std::memcpy(buffer.mapped, data, bytes);
            descriptorsDirty = true;
            ++geometryBufferGrowths;
            return true;
        };
        bool verticesGrew =
            upload(vertexData, geometryVertices.data(), geometryVertices.size() * sizeof(GpuVertex),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        bool indicesGrew =
            upload(indexData, geometryIndices.data(), geometryIndices.size() * sizeof(uint32_t),
                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        for (auto m : added) {
            const auto& a = meshAllocations[m];
            const auto& range = meshes[m];
            if (!verticesGrew)
                std::memcpy(static_cast<GpuVertex*>(vertexData.mapped) + a.firstVertex,
                            geometryVertices.data() + a.firstVertex, a.vertexCount * sizeof(GpuVertex));
            if (!indicesGrew)
                std::memcpy(static_cast<uint32_t*>(indexData.mapped) + range.firstIndex,
                            geometryIndices.data() + range.firstIndex, range.indexCount * sizeof(uint32_t));
        }
        if (!added.empty()) {
            auto c = vk.beginOneTime();
            for (auto m : added)
                buildMeshAS(c, m, false);
            vk.endOneTime(c);
        }
    }
    void initializeGeometry() {
        auto primitives = buildPrimitives(geometryVertices, geometryIndices);
        meshes.assign(primitives.begin(), primitives.end());
        uint32_t capsuleFirst = 24;
        meshAllocations = {{0, capsuleFirst, false},
                           {capsuleFirst, uint32_t(geometryVertices.size()) - capsuleFirst, false}};
        vertexRanges.allocate(uint32_t(geometryVertices.size()));
        indexRanges.allocate(uint32_t(geometryIndices.size()));
        blas.resize(2);
        blasScratch.resize(2);
        vertexCount = uint32_t(geometryVertices.size());
        uploadGeometry({0, 1});
    }
    uint32_t allocateGeometry(const std::vector<GpuVertex>& vertices, const std::vector<uint32_t>& indices,
                              bool dynamic) {
        uint32_t m;
        if (freeMeshes.empty()) {
            m = uint32_t(meshes.size());
            meshes.emplace_back();
            meshAllocations.emplace_back();
            blas.emplace_back();
            blasScratch.emplace_back();
        } else {
            m = freeMeshes.back();
            freeMeshes.pop_back();
        }
        auto firstVertex = vertexRanges.allocate(uint32_t(vertices.size()));
        auto firstIndex = indexRanges.allocate(uint32_t(indices.size()));
        geometryVertices.resize(vertexRanges.size());
        geometryIndices.resize(indexRanges.size());
        std::copy(vertices.begin(), vertices.end(), geometryVertices.begin() + firstVertex);
        for (size_t i = 0; i < indices.size(); ++i)
            geometryIndices[firstIndex + i] = firstVertex + indices[i];
        meshes[m] = {firstIndex, uint32_t(indices.size())};
        meshAllocations[m] = {firstVertex, uint32_t(vertices.size()), dynamic};
        vertexCount = uint32_t(geometryVertices.size());
        return m;
    }
    void releaseGeometry(uint32_t m) {
        // Render fence has completed. Old immutable snapshots may still own assets,
        // but only the current render mirror owns these GPU allocations.
        destroyAS(blas[m]);
        vk.destroy(blasScratch[m]);
        vertexRanges.release(meshAllocations[m].firstVertex, meshAllocations[m].vertexCount);
        indexRanges.release(meshes[m].firstIndex, meshes[m].indexCount);
        freeMeshes.push_back(m);
        ++geometryReleases;
    }
    void syncGeometry(const Frame& frame) {
        std::set<std::shared_ptr<const StaticMesh>> wanted;
        for (const auto& draw : frame.staticMeshes)
            wanted.insert(draw.mesh);
        for (auto it = staticAssets.begin(); it != staticAssets.end();)
            if (!wanted.count(it->first)) {
                releaseGeometry(it->second);
                it = staticAssets.erase(it);
            } else
                ++it;
        std::map<uint32_t, const Frame::Skin*> skins;
        for (const auto& skin : frame.skins)
            skins.emplace(skin.slot, &skin);
        for (auto it = skinDraws.begin(); it != skinDraws.end();) {
            auto skin = skins.find(it->first);
            if (skin == skins.end() || skin->second->mesh != it->second.mesh ||
                frame.proxies[it->first].attributes.entity != it->second.entity) {
                releaseGeometry(it->second.meshIndex);
                it = skinDraws.erase(it);
            } else
                ++it;
        }
        std::vector<uint32_t> added;
        for (const auto& mesh : wanted)
            if (!staticAssets.count(mesh)) {
                std::vector<GpuVertex> vertices;
                vertices.reserve(mesh->vertices.size());
                for (const auto& v : mesh->vertices)
                    vertices.push_back({vec4(v.position, 1), vec4(v.normal, 0), vec4(v.color, 1),
                                        vec4(v.position, 1), vec4(v.uv, 0, 0), v.tangent});
                auto m = allocateGeometry(vertices, mesh->indices, false);
                staticAssets.emplace(mesh, m);
                added.push_back(m);
            }
        staticSlots.clear();
        for (const auto& draw : frame.staticMeshes)
            staticSlots.emplace(draw.slot, staticAssets.at(draw.mesh));
        for (const auto& skin : frame.skins)
            if (!skinDraws.count(skin.slot)) {
                auto deformed = deformSkin(*skin.mesh, skin.palette);
                std::vector<GpuVertex> vertices;
                vertices.reserve(deformed.size());
                for (size_t i = 0; i < deformed.size(); ++i)
                    vertices.push_back({vec4(deformed[i].position, 1), vec4(deformed[i].normal, 0),
                                        vec4(skin.mesh->vertices[i].color, 1),
                                        vec4(deformed[i].position, 1)});
                auto m = allocateGeometry(vertices, skin.mesh->indices, true);
                skinDraws.emplace(skin.slot, SkinDraw{frame.proxies[skin.slot].attributes.entity, m,
                                                      skin.mesh, skin.palette});
                added.push_back(m);
            }
        skinMeshes.clear();
        for (const auto& draw : skinDraws)
            skinMeshes.emplace(draw.first, draw.second.meshIndex);
        if (!added.empty())
            uploadGeometry(added);
        geometryTopology = frame.delta.topology;
    }
    void updateSkins(const Frame& frame) {
        if (geometryTopology != frame.delta.topology)
            syncGeometry(frame);
        dirtySkinMeshes.clear();
        for (const auto& skin : frame.skins) {
            auto& draw = skinDraws.at(skin.slot);
            bool changed = draw.palette != skin.palette;
            if (!changed && !draw.settling && !frame.resetHistory)
                continue;
            auto first = meshAllocations[draw.meshIndex].firstVertex;
            std::vector<DeformedVertex> deformed;
            if (changed)
                deformed = deformSkin(*skin.mesh, skin.palette);
            for (size_t j = 0; j < skin.mesh->vertices.size(); ++j) {
                auto& v = geometryVertices[first + j];
                v.previousPosition = v.position;
                if (changed) {
                    v.position = vec4(deformed[j].position, 1);
                    v.normal = vec4(deformed[j].normal, 0);
                }
                if (frame.resetHistory)
                    v.previousPosition = v.position;
            }
            std::memcpy(static_cast<GpuVertex*>(vertexData.mapped) + first, geometryVertices.data() + first,
                        skin.mesh->vertices.size() * sizeof(GpuVertex));
            if (changed)
                dirtySkinMeshes.push_back(draw.meshIndex);
            draw.settling = changed;
            draw.palette = skin.palette;
        }
    }
    void updateTextures(const std::vector<std::shared_ptr<const TextureAsset>>& textures) {
        if (materialSampler && textures == textureBindings)
            return;
        if (textures.size() > MaxTextures)
            throw std::runtime_error("Map exceeds the material texture descriptor capacity");
        auto oldBindings = std::move(textureBindings);
        auto oldImages = std::move(materialTextures);
        textureBindings = textures;
        materialTextures = {};
        if (!materialSampler) {
            VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            info.magFilter = info.minFilter = VK_FILTER_LINEAR;
            info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            info.anisotropyEnable = VK_TRUE;
            info.maxAnisotropy = std::min(8.f, vk.properties.limits.maxSamplerAnisotropy);
            info.maxLod = VK_LOD_CLAMP_NONE;
            VK_CHECK(vkCreateSampler(vk.device, &info, nullptr, &materialSampler));
        }
        auto upload = [&](uint32_t w, uint32_t h, const uint32_t* pixels, bool srgb) {
            uint32_t levels = 1 + uint32_t(std::floor(std::log2(std::max(w, h))));
            Image texture = vk.image(w, h, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     levels);
            auto staging =
                vk.buffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload);
            std::memcpy(staging.mapped, pixels, size_t(staging.size));
            auto c = vk.beginOneTime();
            vk.transition(c, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(c, staging.handle, texture.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);
            auto mipBarrier = [&](uint32_t level, VkImageLayout before, VkImageLayout after) {
                VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
                barrier.oldLayout = before;
                barrier.newLayout = after;
                barrier.image = texture.handle;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(c, &dependency);
            };
            int32_t mw = int32_t(w), mh = int32_t(h);
            for (uint32_t level = 1; level < levels; ++level) {
                mipBarrier(level - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkImageBlit region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                region.srcOffsets[1] = {mw, mh, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                region.dstOffsets[1] = {std::max(1, mw / 2), std::max(1, mh / 2), 1};
                vkCmdBlitImage(c, texture.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
                mipBarrier(level - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                mw = std::max(1, mw / 2);
                mh = std::max(1, mh / 2);
            }
            mipBarrier(levels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vk.endOneTime(c);
            vk.destroy(staging);
            materialTextures.push_back(texture);
        };
        for (const auto& texture : textures) {
            auto found = std::find(oldBindings.begin(), oldBindings.end(), texture);
            if (found != oldBindings.end()) {
                auto& retained = oldImages[size_t(found - oldBindings.begin())];
                materialTextures.push_back(retained);
                retained = {};
            } else
                upload(texture->width, texture->height, texture->pixels.data(), texture->srgb);
        }
        if (textures.empty()) {
            const uint32_t white = 0xffffffffu;
            upload(1, 1, &white, false);
        }
        for (auto& old : oldImages)
            vk.destroy(old);
        descriptorsDirty = true;
        historyValid = false;
    }
    void createPipelines() {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (uint32_t b = 0; b <= 38; b++) {
            VkDescriptorType type = b == 29  ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                    : b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                    : b <= 5 || (b >= 13 && b <= 18) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                    : b == 6 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                             : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings.push_back(
                {b, type, b == 29 ? MaxTextures : 1,
                 VkShaderStageFlags(b <= 3 || b == 29 ? VK_SHADER_STAGE_ALL : VK_SHADER_STAGE_COMPUTE_BIT),
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
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 25},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxTextures}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1;
        dp.poolSizeCount = 5;
        dp.pPoolSizes = poolSizes;
        VK_CHECK(vkCreateDescriptorPool(vk.device, &dp, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptorPool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &setLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk.device, &da, &descriptors));
        compute[Composite] = createComputePipeline(loadShader(vk, "composite.comp"));
        compute[DiGradientFilter] = createComputePipeline(loadShader(vk, "di_gradient_filter.comp"));
        compute[DiConfidence] = createComputePipeline(loadShader(vk, "di_confidence.comp"));
    }
    VkPipeline createComputePipeline(VkShaderModule module) {
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.layout = pipelineLayout;
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = module;
        cp.stage.pName = "main";
        VkPipeline pipeline;
        auto result = vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline);
        vkDestroyShaderModule(vk.device, module, nullptr);
        VK_CHECK(result);
        return pipeline;
    }
    void updateMaterials(const Frame& frame) {
        auto bindings = MaterialBindings::build(frame.materials);
        for (const auto& shader : bindings.shaders) {
            if (!rasterPrograms.count(shader))
                rasterPrograms.emplace(shader, createRasterPipeline(shader));
        }
        auto programs = computePrograms.find(bindings.shaders);
        if (programs == computePrograms.end()) {
            std::array<VkPipeline, ShaderCompiler::surfacePasses.size()> pipelines{};
            const auto& names = ShaderCompiler::surfacePasses;
            try {
                for (uint32_t i = 0; i < pipelines.size(); ++i) {
                    auto module = createShaderModule(vk, shaderCompiler.compile(names[i], bindings.shaders));
                    pipelines[i] = createComputePipeline(module);
                }
            } catch (...) {
                for (auto pipeline : pipelines)
                    if (pipeline)
                        vkDestroyPipeline(vk.device, pipeline, nullptr);
                throw;
            }
            programs = computePrograms.emplace(bindings.shaders, pipelines).first;
        }
        updateTextures(bindings.textures);
        std::copy(programs->second.begin(), programs->second.end(), compute.begin());
        materialBindings = std::move(bindings);
        std::memcpy(materialData.mapped, materialBindings.materials.data(),
                    materialBindings.materials.size() * sizeof(GpuMaterial));
    }
    VkPipeline createRasterPipeline(const std::shared_ptr<const ShaderAsset>& shader) {
        const auto& code = shaderCompiler.compile("gbuffer.frag", {shader});
        VkShaderModule vertex = loadShader(vk, "gbuffer.vert"), fragment = createShaderModule(vk, code);
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
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
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
        gp.layout = pipelineLayout;
        VkPipeline raster;
        auto result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &gp, nullptr, &raster);
        vkDestroyShaderModule(vk.device, vertex, nullptr);
        vkDestroyShaderModule(vk.device, fragment, nullptr);
        VK_CHECK(result);
        return raster;
    }
    static const char* presentName(VkPresentModeKHR mode) {
        return mode == VK_PRESENT_MODE_MAILBOX_KHR     ? "MAILBOX"
               : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
                                                       : "FIFO";
    }
    // FIFO is the only mode Vulkan guarantees, so an unsupported request degrades to it
    // rather than failing to start.
    VkPresentModeKHR selectPresentMode() const {
        VkPresentModeKHR wanted = options.present == PresentMode::Mailbox     ? VK_PRESENT_MODE_MAILBOX_KHR
                                  : options.present == PresentMode::Immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
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
        descriptorsDirty = true; // Every screen-sized image is about to be replaced.
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
        vk.destroy(readback);
        vk.destroy(auditReadback);
        // A resize starts a new image-sized audit, as with the temporal histories.
        if (audit)
            audit->finish();
        audit.reset();
        for (uint32_t b = 7; b <= 38; b++)
            if (!(b >= 13 && b <= 18) && b != 29) {
                VkFormat imageFormat = b == 9 || b == 25    ? VK_FORMAT_R32G32B32A32_SFLOAT
                                       : b == 11 || b == 31 ? VK_FORMAT_R32_SFLOAT
                                       : b == 33 || b == 34 ? VK_FORMAT_R16_SFLOAT
                                       : b == 37            ? VK_FORMAT_R16G16_SFLOAT
                                       : b == 28            ? VK_FORMAT_R8G8B8A8_UNORM
                                                            : VK_FORMAT_R16G16B16A16_SFLOAT;
                images[b] =
                    vk.image(b == 32 || b == 38 ? (width + 2) / 3 : width,
                             b == 32 || b == 38 ? (height + 2) / 3 : height, imageFormat,
                             ColorUsage | (b <= 12 || b == 28 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0));
            }
        depth = vk.image(width, height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        captureImage = vk.image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT);
        VkDeviceSize pixels = VkDeviceSize(width) * height;
        // RTXDI uses four packed 24-byte arrays, matching the old DI's 96 bytes/pixel.
        const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        constexpr uint32_t block = RTXDI_RESERVOIR_BLOCK_SIZE;
        const VkDeviceSize diLayerBytes = VkDeviceSize((width + block - 1) / block) *
                                          ((height + block - 1) / block) * block * block *
                                          sizeof(RTXDI_PackedDIReservoir);
        reservoirs[0] = vk.buffer(diLayerBytes * 4, usage);
        reservoirs[1] =
            vk.buffer(1024 * sizeof(vec2), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
        reservoirs[2] = vk.buffer(MaxLights * (sizeof(vec4) + sizeof(Light)),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
        auto* offsets = static_cast<vec2*>(reservoirs[1].mapped);
        for (uint32_t i = 0; i < 1024; ++i) {
            float radius = std::sqrt((i + .5f) / 1024.f), angle = i * 2.39996323f;
            offsets[i] = radius * vec2(std::cos(angle), std::sin(angle));
        }
        for (uint32_t i = 3; i < 6; ++i)
            reservoirs[i] = vk.buffer(pixels * 64, usage);
        readback = vk.buffer(pixels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
        if (!options.audit.empty()) {
            auditReadback = vk.buffer(pixels * 8 * RenderAudit::signalCount, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      BufferMemory::Readback);
            audit = std::make_unique<RenderAuditWorker>(width, height);
        }
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
    void updateDescriptors() {
        std::array<VkDescriptorBufferInfo, 39> buffers{};
        std::array<VkDescriptorImageInfo, 39> textureInfo{};
        std::vector<VkWriteDescriptorSet> writes;
        std::array<VkDescriptorImageInfo, MaxTextures> sampled{};
        for (uint32_t i = 0; i < MaxTextures; ++i)
            sampled[i] = {materialSampler,
                          materialTextures[std::min(size_t(i), materialTextures.size() - 1)].view,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &tlas.handle;
        Buffer* basics[] = {&globals, &instanceData, &materialData, &lightData, &vertexData, &indexData};
        for (uint32_t b = 0; b <= 38; b++) {
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
            } else if (b == 29) {
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.descriptorCount = MaxTextures;
                w.pImageInfo = sampled.data();
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
    GpuInstance* instances() {
        return static_cast<GpuInstance*>(instanceData.mapped);
    }
    VkAccelerationStructureInstanceKHR* accelerationInstances() {
        return static_cast<VkAccelerationStructureInstanceKHR*>(tlasInstances.mapped);
    }
    static VkTransformMatrixKHR rowMajor(const mat4& model) {
        VkTransformMatrixKHR rows;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++)
                rows.matrix[r][c] = model[c][r];
        return rows;
    }
    // Transform fast path: touches instance bytes [0, 128) and acceleration structure
    // instance bytes [0, 48). Geometry binding, material, entity id and visibility mask
    // are left exactly as they were, so moving an object never reconsiders what it is.
    void writeTransform(uint32_t slot, const RenderProxy& p, bool zeroMotion) {
        const mat4 model = skinMeshes.count(slot) ? mat4(1) : transform(p.transform);
        const mat4 before = zeroMotion ? model : shadowModel[slot];
        auto& instance = instances()[slot];
        instance.previousModel = before;
        instance.model = model;
        shadowModel[slot] = model;
        accelerationInstances()[slot].transform = rowMajor(model);
        // Only a slot that is actually reporting motion has to be settled once it stops.
        if (before != model && motionFrame[slot] != sceneStamp) {
            motionFrame[slot] = sceneStamp;
            movedThisFrame.push_back(slot);
        }
        sceneStatistics.transforms++;
    }
    // Folds a slot's motion forward without moving it, so an object that moved on the
    // previous frame and then stopped reports no motion instead of repeating the last
    // one. Writes only the previous-transform half.
    void settleMotion(uint32_t slot) {
        instances()[slot].previousModel = shadowModel[slot];
        sceneStatistics.settled++;
    }
    // Attribute path: the instance's trailing uvec4 plus the whole acceleration structure
    // instance, which is where the visibility mask and geometry binding live.
    void writeAttributes(uint32_t slot, const RenderProxy& p) {
        instances()[slot].info = {p.attributes.material, meshes[meshFor(slot, p)].firstIndex,
                                  p.attributes.entity, p.attributes.interactable ? 1u : 0u};
        VkAccelerationStructureInstanceKHR a{};
        a.transform = rowMajor(shadowModel[slot]);
        a.instanceCustomIndex = slot;
        a.mask = p.live && p.attributes.visible ? 0xffu : 0u;
        a.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        if (p.live) {
            auto shaderIndex = materialBindings.materials[p.attributes.material].info.x;
            const auto& state = materialBindings.shaders[shaderIndex]->renderState;
            if (state.mode == SurfaceMode::Opaque && state.cull == SurfaceCull::None)
                a.flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        }
        a.accelerationStructureReference = blas[meshFor(slot, p)].address;
        accelerationInstances()[slot] = a; // Built in cached memory, stored once.
        sceneStatistics.attributes++;
    }
    void writeSlot(uint32_t slot, const RenderProxy& p, bool zeroMotion) {
        writeTransform(slot, p, zeroMotion);
        writeAttributes(slot, p);
    }
    // Brings the GPU mirror in line with a snapshot. The delta is applied when the mirror
    // sits exactly at the revision the delta was built against; otherwise a snapshot was
    // skipped and the only safe recovery is to rewrite every slot from the array, which
    // costs exactly what the engine used to pay on every single frame.
    void applyScene(const Frame& frame, bool reset) {
        const uint32_t capacity = uint32_t(frame.proxies.size());
        sceneStatistics = {};
        sceneStatistics.slots = capacity;
        if (capacity > shadowModel.size()) {
            shadowModel.resize(capacity);
            motionFrame.resize(capacity, 0);
        }
        ++sceneStamp;
        movedThisFrame.clear();
        // The renderer may outrun the simulation and be handed the same snapshot twice.
        // Its scene state is already on the GPU; only motion needs attention.
        const bool repeat =
            mirrorValid && !reset && capacity == mirrorCapacity && frame.delta.revision == mirrorRevision;
        // A reset discards temporal history, so every slot's previous transform has to be
        // folded onto its current one. Otherwise the delta applies only when the mirror
        // sits exactly at the revision the delta was built against; if a snapshot was
        // skipped, the events that came with it are gone and the array is the only truth.
        //
        // Growth is deliberately not a resync trigger. Slots only ever appear through
        // RenderScene::create, which marks them structural, so a chained delta already
        // carries every slot the mirror has not seen; rewriting the ones it has seen
        // would make spawning cost the whole scene.
        const bool resync = !mirrorValid || reset || options.fullUpload || frame.forceFullUpload ||
                            (!repeat && frame.delta.base != mirrorRevision);
        if (resync) {
            // Resynchronising is about which slots to rewrite, not about forgetting where
            // they were. The mirror still holds every slot's previously rendered
            // transform, so motion vectors survive a skipped snapshot; only a slot the
            // mirror has never written, or a discarded history, has no previous state.
            const bool fresh = !mirrorValid || reset;
            for (uint32_t slot = 0; slot < capacity; slot++)
                writeSlot(slot, frame.proxies[slot], fresh || slot >= mirrorCapacity);
        } else if (repeat) {
            for (auto slot : movedLastFrame)
                settleMotion(slot);
        } else {
            for (auto slot : frame.delta.moved)
                writeTransform(slot, frame.proxies[slot], false);
            for (auto slot : frame.delta.attributes)
                writeAttributes(slot, frame.proxies[slot]);
            // Structural changes rewrite both halves, so they run last and win. A slot
            // that just appeared, or was handed to a different object, has no previous
            // position worth interpolating from.
            for (auto slot : frame.delta.structural) {
                writeSlot(slot, frame.proxies[slot], true);
                sceneStatistics.structural++;
            }
            // Anything that moved on the previous frame and has now stopped would keep
            // reporting the motion it had then, smearing the denoiser behind it.
            for (auto slot : movedLastFrame)
                if (motionFrame[slot] != sceneStamp)
                    settleMotion(slot);
        }
        movedLastFrame = movedThisFrame;
        sceneStatistics.resynchronised = resync;
        mirrorRevision = frame.delta.revision;
        mirrorCapacity = capacity;
        mirrorValid = true;
        sceneWrites += sceneStatistics.transforms + sceneStatistics.attributes + sceneStatistics.settled;
        sceneResyncs += resync;
        sceneSlotFrames += capacity;
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
        // The instance array is slot-addressed and covers dead slots too, so the primitive
        // count only moves when the scene gains slots: hiding, showing, moving or
        // destroying an object leaves it alone and takes the incremental path.
        uint32_t count = uint32_t(frame.proxies.size());
        // Storage and scratch depend only on how many instances the structure can hold, so
        // reallocating is reserved for actually outgrowing it. Rebuilding reuses what is
        // already there. Doing otherwise meant freeing and reallocating device memory on
        // any frame that had to rebuild, which is the sort of churn that quietly makes a
        // long session slower than a short one.
        if (!tlas.handle || count > tlasCapacity) {
            uint32_t capacity = std::max(count, 64u);
            capacity = (capacity + 63) & ~63u; // Grow in blocks so spawning is not a cliff.
            destroyAS(tlas);
            vk.destroy(tlasScratch);
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(
                vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &capacity, &sizes);
            tlas = createAS(info.type, sizes.accelerationStructureSize);
            tlasScratch =
                vk.buffer(std::max(sizes.buildScratchSize, sizes.updateScratchSize) +
                              vk.asProperties.minAccelerationStructureScratchOffsetAlignment,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            tlasCapacity = capacity;
            tlasCount = 0;
            tlasBuilt = false;
            descriptorsDirty = true; // The descriptor set still points at the old handle.
        }
        // Refitting cannot absorb a slot appearing or rebinding to different geometry.
        // That arrives as a running count rather than a flag, so it is still detected when
        // the snapshot carrying it was skipped, and no instance reference ever changes
        // underneath an incremental update.
        bool update = tlasBuilt && tlasCount == count && frame.delta.topology == mirrorTopology;
        tlasBuilt = true;
        tlasCount = count;
        mirrorTopology = frame.delta.topology;
        sceneStatistics.tlasRebuilt = !update;
        sceneTlasRebuilds += !update;
        info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                           : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.srcAccelerationStructure = update ? tlas.handle : VK_NULL_HANDLE;
        info.dstAccelerationStructure = tlas.handle;
        info.scratchData.deviceAddress = alignedScratch(tlasScratch);
        VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
        const auto* rangePointer = &range;
        GpuScope scope(*profiler, c, update ? "TLAS Refit" : "TLAS Build");
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
        VkPipeline boundPipeline = VK_NULL_HANDLE;
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptors, 0,
                                nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(c, 0, 1, &vertexData.handle, &offset);
        vkCmdBindIndexBuffer(c, indexData.handle, 0, VK_INDEX_TYPE_UINT32);
        // The slot is the draw's instance index, so gl_InstanceIndex, the acceleration
        // structure's custom index and the GPU instance entry all stay the same number.
        for (uint32_t slot = 0; slot < frame.proxies.size(); slot++) {
            const auto& p = frame.proxies[slot];
            if (!p.live || !p.attributes.visible)
                continue;
            auto shader = frame.materials[p.attributes.material].shader;
            auto pipeline = rasterPrograms.at(shader);
            if (pipeline != boundPipeline) {
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                boundPipeline = pipeline;
            }
            auto& mesh = meshes[meshFor(slot, p)];
            vkCmdDrawIndexed(c, mesh.indexCount, 1, mesh.firstIndex, 0, slot);
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
        uint32_t w = pipeline == DiGradient || pipeline == DiGradientFilter ? (width + 2) / 3 : width;
        uint32_t h = pipeline == DiGradient || pipeline == DiGradientFilter ? (height + 2) / 3 : height;
        vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    }
    void copyHistory(VkCommandBuffer c) {
        for (auto pair : {std::pair<uint32_t, uint32_t>{8, 24}, {9, 25}, {7, 30}, {11, 31}, {35, 36}}) {
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
        const VkDeviceSize layer = reservoirs[0].size / 4;
        VkBufferCopy diCopies[] = {{2 * layer, 0, layer}, {layer, 3 * layer, layer}};
        vkCmdCopyBuffer(c, reservoirs[0].handle, reservoirs[0].handle, 2, diCopies);
        VkBufferCopy giCopy{0, 0, reservoirs[3].size};
        vkCmdCopyBuffer(c, reservoirs[3].handle, reservoirs[4].handle, 1, &giCopy);
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
               << ",\n  \"staticMeshInstances\": " << staticSlots.size()
               << ", \"geometryBuilds\": " << geometryBuilds << ", \"geometryReleases\": " << geometryReleases
               << ", \"geometryBufferGrowths\": " << geometryBufferGrowths
               << ", \"staticMeshAssets\": " << staticAssets.size()
               << ", \"textureAssets\": " << textureBindings.size()
               << ", \"shaderAssets\": " << materialBindings.shaders.size()
               << ", \"shaderCompilations\": " << shaderCompiler.compilationCount()
               << ", \"rasterPrograms\": " << rasterPrograms.size()
               << ",\n  \"sceneSlots\": " << sceneStatistics.slots << ", \"sceneSlotWrites\": " << sceneWrites
               << ", \"sceneSlotWritesIfRebuilt\": " << sceneSlotFrames
               << ",\n  \"sceneResyncs\": " << sceneResyncs << ", \"tlasRebuilds\": " << sceneTlasRebuilds
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
        if (auditReadbackPending) {
            audit->submit(static_cast<const uint16_t*>(auditReadback.mapped));
            auditReadbackPending = false;
        }
    }
    bool render(const FrameRef& sourceFrame) {
        const bool captureCpu =
            sourceFrame->cpuProfile && sourceFrame->cpuProfile->request > lastCpuProfileRequest;
        CpuProfiler cpuProfiler(captureCpu, "Render", "Render Frame");
        FrameRef frameRef = sourceFrame;
        if (options.auditMotion || options.auditOccluder >= 0) {
            Frame diagnostic = *sourceFrame;
            if (options.auditMotion)
                diagnostic.camera.yaw += .04f * std::sin(float(frameNumber) * .017f);
            if (options.auditOccluder >= 0) {
                diagnostic.proxies.at(size_t(options.auditOccluder)).transform.position.x +=
                    frameNumber >= 64 ? 2.f : 0.f;
                diagnostic.forceFullUpload = true;
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
            updateSkins(frame);
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
        if (materialsChanged)
            updateMaterials(frame);
        Globals data{};
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
        data.renderSettings = {frame.exposure, frame.diHistoryConfidence ? 1.f : 0.f,
                               lightsChanged ? 1.f : 0.f, 0};
        data.counts = {uint32_t(frame.proxies.size()), uint32_t(frame.lights.size()), uint32_t(frameNumber),
                       reset ? 0u : 1u};
        std::memcpy(globals.mapped, &data, sizeof(data));
        applyScene(frame, reset);
        // Materials and lights are compared against the previous snapshot anyway, to
        // decide whether temporal history survives; the same answer decides whether they
        // are worth uploading again.
        if (lightsChanged)
            std::memcpy(lightData.mapped, frame.lights.data(), frame.lights.size() * sizeof(Light));
        // The discrete proposal is a power/uniform mixture. Keep every light reachable.
        auto* distribution = static_cast<vec4*>(reservoirs[2].mapped);
        float totalPower = 0;
        for (const auto& light : frame.lights)
            totalPower +=
                glm::dot(vec3(light.colorIntensity), vec3(.2126f, .7152f, .0722f)) * light.colorIntensity.w;
        float cdf = 0;
        for (size_t i = 0; i < frame.lights.size(); ++i) {
            const auto& light = frame.lights[i];
            float power =
                glm::dot(vec3(light.colorIntensity), vec3(.2126f, .7152f, .0722f)) * light.colorIntensity.w;
            float pdf = totalPower > 0 ? .9f * power / totalPower + .1f / frame.lights.size()
                                       : 1.f / frame.lights.size();
            cdf += pdf;
            distribution[i] = vec4(i + 1 == frame.lights.size() ? 1.f : cdf, pdf, 0, 0);
        }
        const auto& oldLights = reset ? frame.lights : previous->lights;
        std::memcpy(distribution + MaxLights, oldLights.data(), oldLights.size() * sizeof(Light));
        uploadScope.finish();
        {
            CpuScope scope("UI / Prepare Resources");
            if (!uiRenderer)
                uiRenderer = std::make_unique<UiRenderer>(vk);
            uiRenderer->prepare(frame.ui.get());
        }
        CpuScope recordScope("Record GPU Commands");
        VK_CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command, &begin));
        const auto request = frame.gpuProfileRequest > lastProfileRequest ? frame.gpuProfileRequest : 0;
        profiler->beginFrame(command, request, frameNumber + 1, width, height);
        if (request)
            lastProfileRequest = request;
        if (!historyValid) {
            CpuScope cpuScope("Initialize History / Reservoirs");
            GpuScope scope(*profiler, command, "Initialize History / Reservoirs");
            VkClearColorValue clear{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            for (auto& image : images)
                if (image.handle) {
                    vk.transition(command, image, VK_IMAGE_LAYOUT_GENERAL);
                    vkCmdClearColorImage(command, image.handle, image.layout, &clear, 1, &range);
                }
            for (auto i : {0, 3, 4, 5})
                vkCmdFillBuffer(command, reservoirs[i].handle, 0, VK_WHOLE_SIZE, 0);
            VulkanContext::barrier(command);
        }
        {
            CpuScope cpuScope("UI / Draw");
            GpuScope scope(*profiler, command, "RmlUi Overlay");
            uiRenderer->draw(command, images[28], frame.ui.get());
        }
        // TLAS allocation precedes descriptor writes; actual construction is recorded before raster/compute.
        if (!dirtySkinMeshes.empty()) {
            CpuScope cpuScope("Skinned BLAS Refit");
            GpuScope scope(*profiler, command, "Skinned BLAS Refit");
            for (auto m : dirtySkinMeshes)
                buildMeshAS(command, m, true);
            VulkanContext::barrier(command);
            dirtySkinMeshes.clear();
        }
        {
            CpuScope cpuScope("Acceleration Structures / TLAS");
            GpuScope scope(*profiler, command, "Acceleration Structures");
            buildTLAS(command, frame);
            VulkanContext::barrier(command);
        }
        // Bindings name resources, not their contents. Nothing but a swapchain resize or
        // a rebuilt acceleration structure replaces a handle, so the set is written once
        // and then left alone instead of being rewritten every frame.
        if (descriptorsDirty) {
            CpuScope scope("Update Descriptors");
            updateDescriptors();
            descriptorsDirty = false;
        }
        RenderGraph graph;
        graph.add("GBuffer Raster", [&](auto c) { rasterize(c, frame); });
        graph.add("RTXDI Same Sample Gradient", [&](auto c) { dispatch(c, DiGradient); });
        graph.add("RTXDI Gradient Filter", [&](auto c) { dispatch(c, DiGradientFilter); });
        graph.add("RTXDI History Confidence", [&](auto c) { dispatch(c, DiConfidence); });
        graph.add("RTXDI Initial + Secondary GI + Specular", [&](auto c) { dispatch(c, Lighting); });
        graph.add("RTXDI Temporal Resampling", [&](auto c) { dispatch(c, DiTemporal); });
        graph.add("RTXDI Spatial Resampling", [&](auto c) { dispatch(c, DiSpatial); });
        graph.add("ReSTIR GI Reconnection", [&](auto c) { dispatch(c, GiReuse); });
        graph.add("Visibility + Radiance Resolve", [&](auto c) { dispatch(c, Resolve); });
        graph.add("NRD RELAX Diffuse Specular", [&](auto c) {
            std::array<Image*, size_t(nrd::ResourceType::MAX_NUM)> resources{};
            resources[size_t(nrd::ResourceType::IN_MV)] = &images[10];
            resources[size_t(nrd::ResourceType::IN_NORMAL_ROUGHNESS)] = &images[8];
            resources[size_t(nrd::ResourceType::IN_VIEWZ)] = &images[11];
            resources[size_t(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST)] = &images[19];
            resources[size_t(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST)] = &images[20];
            resources[size_t(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST)] = &images[21];
            resources[size_t(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST)] = &images[22];
            resources[size_t(nrd::ResourceType::IN_DIFF_CONFIDENCE)] = &images[33];
            resources[size_t(nrd::ResourceType::IN_SPEC_CONFIDENCE)] = &images[34];
            denoiser->dispatch(c, resources, frame.camera, uint32_t(frameNumber), reset, frameMs, *profiler);
        });
        graph.add("Composition + Tone Map + HUD", [&](auto c) { dispatch(c, Composite); });
        graph.add("History Store", [&](auto c) { copyHistory(c); });
        graph.execute(command, *profiler);
        bool auditFrame = !options.audit.empty() && frameNumber >= 64;
        if (auditFrame) {
            CpuScope cpuScope("Audit Readback Commands");
            GpuScope scope(*profiler, command, "Audit Readback");
            for (uint32_t s = 0; s < RenderAudit::signalCount; ++s) {
                auto& image = images[RenderAudit::bindings[s]];
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
            CpuScope cpuScope("Screenshot Readback Commands");
            GpuScope scope(*profiler, command, "Screenshot Readback");
            blit(command, images[23], captureImage);
            vk.transition(command, captureImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            vkCmdCopyImageToBuffer(command, captureImage.handle, captureImage.layout, readback.handle, 1,
                                   &copy);
        }
        {
            CpuScope cpuScope("Swapchain Blit / Present Transition");
            GpuScope scope(*profiler, command, "Swapchain Blit / Present Transition");
            blit(command, images[23], swapImages[swapIndex]);
            vk.transition(command, swapImages[swapIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_PIPELINE_STAGE_2_NONE, 0);
            vk.transition(command, images[23], VK_IMAGE_LAYOUT_GENERAL);
        }
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
    return impl_->sceneStatistics;
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
