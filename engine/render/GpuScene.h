#pragma once
#include "renderCore/vulkan/VulkanAccess.h"
#include "RasterDraw.h"
#include "Geometry.h"
#include "MaterialBindings.h"
#include "renderCore/RangeAllocator.h"
#include "RenderResources.h"
#include "Renderer.h"
#include "renderCore/vulkan/VulkanContext.h"
#include "animation/SkinnedMesh.h"
#include "assets/StaticMesh.h"
#include <map>
#include <utility>
#include <memory>

namespace whimsical {
class GpuProfiler;

// Everything the scene owns on the GPU: geometry, acceleration structures, the instance
// mirror, material data and textures. Sized by content rather than by the screen, which is
// exactly why the render graph imports these instead of allocating them.
class GpuScene {
  public:
    GpuScene(VulkanContext&, const RenderOptions&);
    ~GpuScene();

    // Registered with the pool once; the graph then names them like any other resource.
    void bind(rc::NativeResources, const SceneResources&);

    void updateMaterials(const MaterialBindings&);
    void updateSkins(const Frame&);
    void writeGlobals(const void* data, size_t bytes);
    void writeOutlines(const std::vector<EntityOutline>&);
    void writeLights(const std::vector<Light>&);
    void writeLightDistribution(const std::vector<Light>& current, const std::vector<Light>& previous);
    void apply(const Frame&, bool reset);

    // Recorded inside graph passes; the graph owns the ordering against the ray queries.
    // Allocation is deliberately not part of recording: the structure object has to exist
    // before the frame's descriptors are written, and recording happens after that.
    bool skinsDirty() const {
        return !dirtySkinMeshes.empty();
    }
    // Whether this frame's top-level build keeps the structure it targets or replaces it,
    // decided by reserveTlas before the frame is declared.
    bool tlasRefits() const {
        return tlasUpdate;
    }
    void recordSkinnedBlas(VkCommandBuffer);
    void recordTlas(VkCommandBuffer, GpuProfiler&);
    // Prepare every pass before uploading once; recording never allocates or sorts.
    void beginDraws();
    std::vector<RasterBatch> prepareDraws(std::vector<RasterDraw>);
    void uploadDraws();
    void recordDraws(VkCommandBuffer, const std::vector<RasterBatch>&);

    // A new texture set replaces images the temporal history was accumulated against.

    SceneUpdateStatistics statistics;
    uint64_t writes = 0, resyncs = 0, tlasRebuilds = 0, slotFrames = 0;
    uint64_t geometryBuilds = 0, geometryReleases = 0, geometryBufferGrowths = 0;
    uint64_t instanceCapacityGrowths = 0;
    size_t staticMeshInstances() const {
        size_t count = 0;
        for (const auto& entry : staticSlots)
            count += proxyInstances[entry.first].count;
        return count;
    }
    size_t staticMeshAssets() const {
        return staticAssets.size();
    }
    size_t textureAssets() const {
        return textureBindings.size();
    }
    uint32_t gpuInstanceCount() const {
        return instanceCount;
    }

    Buffer globals, outlineData, instanceData, materialData, lightData, vertexData, indexData, lightDistribution;

  private:
    struct AccelerationStructure {
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        Buffer storage;
        VkDeviceAddress address = 0;
    };
    struct MeshAllocation {
        uint32_t firstVertex = 0, vertexCount = 0;
        bool dynamic = false;
    };
    struct SkinDraw {
        uint32_t entity = 0, meshIndex = 0;
        std::shared_ptr<const SkinnedMesh> mesh;
        std::vector<mat4> palette;
        bool settling = false;
    };
    VulkanContext& vk;
    const RenderOptions& options;
    std::optional<rc::NativeResources> pool_;
    const SceneResources* ids_ = nullptr;
    Buffer tlasInstances, tlasScratch;
    Buffer drawInstances;
    std::vector<uint32_t> drawSlots;
    uint32_t drawIdentityCapacity = 0;
    struct InstanceRange {
        uint32_t first = 0, count = 0;
        bool fresh = false;
    };
    RangeAllocator instanceRanges;
    std::vector<InstanceRange> proxyInstances;
    uint32_t instanceCount = 0;
    std::vector<AccelerationStructure> blas;
    std::vector<Buffer> blasScratch;
    AccelerationStructure tlas;
    uint32_t tlasCount = 0, tlasCapacity = 0;
    bool tlasBuilt = false, tlasUpdate = false;
    VkAccelerationStructureGeometryKHR tlasGeometry{};
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{};
    std::vector<MeshRange> meshes;
    std::vector<MeshAllocation> meshAllocations;
    std::vector<uint32_t> freeMeshes, geometryIndices, dirtySkinMeshes;
    RangeAllocator vertexRanges, indexRanges;
    std::map<std::shared_ptr<const StaticMesh>, uint32_t> staticAssets;
    std::map<uint32_t, SkinDraw> skinDraws;
    std::map<uint32_t, uint32_t> skinMeshes, staticSlots;
    uint64_t geometryTopology = UINT64_MAX;
    std::vector<Image> materialTextures;
    VkSampler materialSampler = VK_NULL_HANDLE;
    std::vector<std::shared_ptr<const TextureAsset>> textureBindings;
    std::vector<GpuVertex> geometryVertices;
    MaterialBindings bindings;
    // Persistent mirror of the render scene. The renderer keeps its own copy of every
    // slot's model matrix because the GPU-side instance buffer lives in write-combined
    // memory, where reading back what was written costs far more than storing it twice.
    std::vector<mat4> shadowModel;
    std::vector<uint64_t> motionFrame; // Stamp of the update in which a slot last moved.
    std::vector<uint32_t> movedThisFrame, movedLastFrame;
    uint64_t sceneStamp = 0, mirrorRevision = 0, mirrorTopology = 0;
    uint32_t mirrorCapacity = 0;
    bool mirrorValid = false, rayPoliciesDirty = false;

    void destroyAS(AccelerationStructure&);
    AccelerationStructure createAS(VkAccelerationStructureTypeKHR, VkDeviceSize);
    VkDeviceAddress alignedScratch(const Buffer&) const;
    uint32_t allocateGeometry(const std::vector<GpuVertex>&, const std::vector<uint32_t>&, bool dynamic);
    void releaseGeometry(uint32_t);
    void syncGeometry(const Frame&);
    void reserveInstances(size_t count);
    void syncInstances(const Frame&);
    void reserveTlas(const Frame&);
    void uploadGeometry(const std::vector<uint32_t>& added);
    void buildMeshAS(VkCommandBuffer, uint32_t mesh, bool update);
    void updateTextures(const std::vector<std::shared_ptr<const TextureAsset>>&);
    void rebind();
    std::optional<uint32_t> meshFor(uint32_t slot) const;
    void writeTransform(uint32_t slot, const RenderProxy&, bool zeroMotion);
    void settleMotion(uint32_t slot);
    void writeAttributes(uint32_t slot, const RenderProxy&);
    void writeSlot(uint32_t slot, const RenderProxy&, bool zeroMotion);
};
} // namespace whimsical
