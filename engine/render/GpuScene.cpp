#include "GpuScene.h"
#include "renderCore/GpuProfiler.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace whimsical {
namespace {
struct alignas(16) GpuInstance {
    mat4 model, previousModel;
    glm::uvec4 info;
};
static_assert(sizeof(GpuInstance) == 144 && sizeof(GpuMaterial) == 176 && sizeof(GpuVertex) == 96,
              "GPU layout mismatch");
} // namespace

GpuScene::GpuScene(VulkanContext& context, const RenderOptions& opts) : vk(context), options(opts) {
    globals = vk.buffer(sizeof(GpuGlobals), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, BufferMemory::Upload);
    outlineData = vk.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    materialData = vk.buffer(sizeof(GpuMaterial) * MaxMaterials, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             BufferMemory::Upload);
    lightData =
        vk.buffer(sizeof(Light) * MaxLights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    lightDistribution = vk.buffer(MaxLights * (sizeof(vec4) + sizeof(Light)),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    reserveInstances(0);
    drawInstances = vk.buffer(64 * sizeof(uint32_t), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, BufferMemory::Upload);
    uploadGeometry({});
    updateTextures({});
}

GpuScene::~GpuScene() {
    for (auto& texture : materialTextures)
        vk.destroy(texture);
    if (materialSampler)
        vkDestroySampler(vk.device, materialSampler, nullptr);
    for (auto* b : {&globals, &outlineData, &instanceData, &materialData, &lightData, &vertexData, &indexData,
                    &lightDistribution, &tlasInstances, &tlasScratch, &drawInstances})
        vk.destroy(*b);
    for (auto& a : blas)
        destroyAS(a);
    for (auto& b : blasScratch)
        vk.destroy(b);
    destroyAS(tlas);
}

void GpuScene::bind(rc::NativeResources pool, const SceneResources& ids) {
    pool_.emplace(pool);
    ids_ = &ids;
    rebind();
}

// Buffer wrappers carry allocation generations, so growing them in place invalidates
// bindings even when Vulkan recycles a handle. TLAS imports carry their storage generation;
// sampler arrays have an import revision because they contain several separate objects.
void GpuScene::rebind() {
    if (!pool_)
        return;
    pool_->importBuffer(ids_->globals, globals);
    pool_->importBuffer(ids_->outlines, outlineData);
    pool_->importBuffer(ids_->instances, instanceData);
    pool_->importBuffer(ids_->materials, materialData);
    pool_->importBuffer(ids_->lights, lightData);
    pool_->importBuffer(ids_->vertices, vertexData);
    pool_->importBuffer(ids_->indices, indexData);
    pool_->importBuffer(ids_->drawInstances, drawInstances);
    pool_->importBuffer(ids_->buildInstances, tlasInstances);
    pool_->importTlas(ids_->tlas, tlas.handle, tlas.storage.generation);
    std::vector<VkDescriptorImageInfo> sampled(MaxTextures);
    for (uint32_t i = 0; i < MaxTextures; ++i)
        sampled[i] = {materialSampler,
                      materialTextures[std::min(size_t(i), materialTextures.size() - 1)].view,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    pool_->importSamplers(ids_->textures, std::move(sampled));
}

void GpuScene::destroyAS(AccelerationStructure& a) {
    if (a.handle)
        vkDestroyAccelerationStructureKHR(vk.device, a.handle, nullptr);
    vk.destroy(a.storage);
    a = {};
}

GpuScene::AccelerationStructure GpuScene::createAS(VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
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

VkDeviceAddress GpuScene::alignedScratch(const Buffer& buffer) const {
    VkDeviceSize alignment = vk.asProperties.minAccelerationStructureScratchOffsetAlignment;
    return (buffer.address + alignment - 1) / alignment * alignment;
}

std::optional<uint32_t> GpuScene::meshFor(uint32_t slot) const {
    auto mesh = staticSlots.find(slot);
    if (mesh != staticSlots.end())
        return mesh->second;
    auto found = skinMeshes.find(slot);
    if (found != skinMeshes.end())
        return found->second;
    return std::nullopt;
}

void GpuScene::buildMeshAS(VkCommandBuffer c, uint32_t m, bool update) {
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

void GpuScene::uploadGeometry(const std::vector<uint32_t>& added) {
    auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    auto upload = [&](Buffer& buffer, const void* data, size_t bytes, VkBufferUsageFlags extra) {
        if (buffer.handle && buffer.size >= bytes)
            return false;
        auto capacity = std::max<VkDeviceSize>(bytes, std::max<VkDeviceSize>(buffer.size * 2, 4096));
        vk.destroy(buffer);
        buffer = vk.buffer(capacity, usage | extra, BufferMemory::Upload);
        if (bytes)
            std::memcpy(buffer.mapped, data, bytes);
        ++geometryBufferGrowths;
        return true;
    };
    bool verticesGrew =
        upload(vertexData, geometryVertices.data(), geometryVertices.size() * sizeof(GpuVertex),
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    bool indicesGrew = upload(indexData, geometryIndices.data(), geometryIndices.size() * sizeof(uint32_t),
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (verticesGrew || indicesGrew)
        rebind();
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
        vk.execute([&](VkCommandBuffer command) {
            for (auto m : added)
                buildMeshAS(command, m, false);
        });
    }
}

uint32_t GpuScene::allocateGeometry(const std::vector<GpuVertex>& vertices,
                                    const std::vector<uint32_t>& indices, bool dynamic) {
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
    return m;
}

void GpuScene::releaseGeometry(uint32_t m) {
    // Render fence has completed. Old immutable snapshots may still own assets,
    // but only the current render mirror owns these GPU allocations.
    destroyAS(blas[m]);
    vk.destroy(blasScratch[m]);
    vertexRanges.release(meshAllocations[m].firstVertex, meshAllocations[m].vertexCount);
    indexRanges.release(meshes[m].firstIndex, meshes[m].indexCount);
    freeMeshes.push_back(m);
    ++geometryReleases;
}

void GpuScene::syncGeometry(const Frame& frame) {
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
            skinDraws.emplace(skin.slot,
                              SkinDraw{frame.proxies[skin.slot].attributes.entity, m, skin.mesh, skin.palette});
            added.push_back(m);
        }
    skinMeshes.clear();
    for (const auto& draw : skinDraws)
        skinMeshes.emplace(draw.first, draw.second.meshIndex);
    if (!added.empty())
        uploadGeometry(added);
    geometryTopology = frame.delta.topology;
}

void GpuScene::updateSkins(const Frame& frame) {
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

void GpuScene::updateTextures(const std::vector<std::shared_ptr<const TextureAsset>>& textures) {
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
        vk.uploadImage(texture, pixels, size_t(w) * h * 4,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
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
    rebind();
}

void GpuScene::updateMaterials(const MaterialBindings& next) {
    updateTextures(next.textures);
    rayPoliciesDirty |= bindings.rayPolicies != next.rayPolicies;
    bindings = next;
    std::memcpy(materialData.mapped, bindings.materials.data(),
                bindings.materials.size() * sizeof(GpuMaterial));
}

void GpuScene::writeGlobals(const void* data, size_t bytes) {
    std::memcpy(globals.mapped, data, bytes);
}

void GpuScene::writeOutlines(const std::vector<EntityOutline>& outlines) {
    struct alignas(16) Entry { glm::uvec4 identity; vec4 color; };
    const auto bytes = sizeof(glm::uvec4) + outlines.size() * sizeof(Entry);
    if (outlineData.size < bytes) {
        vk.destroy(outlineData);
        outlineData = vk.buffer((bytes + 255) & ~size_t(255), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                BufferMemory::Upload);
        rebind();
    }
    const glm::uvec4 count{uint32_t(outlines.size()), 0, 0, 0};
    std::memcpy(outlineData.mapped, &count, sizeof(count));
    auto* output = static_cast<char*>(outlineData.mapped) + sizeof(count);
    for (const auto& outline : outlines) {
        Entry entry{{outline.entity, 0, 0, 0}, outline.color};
        std::memcpy(output, &entry, sizeof(entry));
        output += sizeof(entry);
    }
}
void GpuScene::writeLights(const std::vector<Light>& lights) {
    std::memcpy(lightData.mapped, lights.data(), lights.size() * sizeof(Light));
}

// The discrete proposal is a power/uniform mixture. Keep every light reachable.
void GpuScene::writeLightDistribution(const std::vector<Light>& current,
                                      const std::vector<Light>& previous) {
    auto* distribution = static_cast<vec4*>(lightDistribution.mapped);
    float totalPower = 0;
    for (const auto& light : current)
        totalPower += lightProposalPower(light);
    float cdf = 0;
    for (size_t i = 0; i < current.size(); ++i) {
        float power = lightProposalPower(current[i]);
        float pdf = totalPower > 0 ? .9f * power / totalPower + .1f / current.size()
                                   : 1.f / current.size();
        cdf += pdf;
        distribution[i] = vec4(i + 1 == current.size() ? 1.f : cdf, pdf, 0, 0);
    }
    std::memcpy(distribution + MaxLights, previous.data(), previous.size() * sizeof(Light));
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
void GpuScene::writeTransform(uint32_t slot, const RenderProxy& p, bool zeroMotion) {
    const auto& range = proxyInstances[slot];
    const auto world = transform(p.transform);
    const bool skinned = skinMeshes.count(slot) != 0;
    // Skin vertices are already in owner-world space. Apply each local instance
    // around that owner, sharing the same deformed geometry and BLAS.
    const auto skinToLocal = skinned && p.instanceTransforms ? glm::inverse(world) : mat4(1);
    for (uint32_t i = 0; i < range.count; ++i) {
        const auto index = range.first + i;
        const mat4 model = p.instanceTransforms ? world * transform((*p.instanceTransforms)[i]) * skinToLocal
                                                : (skinned ? mat4(1) : world);
        const mat4 before = zeroMotion || range.fresh ? model : shadowModel[index];
        auto& instance = static_cast<GpuInstance*>(instanceData.mapped)[index];
        instance.previousModel = before;
        instance.model = model;
        shadowModel[index] = model;
        static_cast<VkAccelerationStructureInstanceKHR*>(tlasInstances.mapped)[index].transform =
            rowMajor(model);
        if (before != model && motionFrame[index] != sceneStamp) {
            motionFrame[index] = sceneStamp;
            movedThisFrame.push_back(index);
        }
        statistics.transforms++;
    }
}

// Folds a slot's motion forward without moving it, so an object that moved on the
// previous frame and then stopped reports no motion instead of repeating the last
// one. Writes only the previous-transform half.
void GpuScene::settleMotion(uint32_t slot) {
    static_cast<GpuInstance*>(instanceData.mapped)[slot].previousModel = shadowModel[slot];
    statistics.settled++;
}

// Attribute path: the instance's trailing uvec4 plus the whole acceleration structure
// instance, which is where the visibility mask and geometry binding live.
void GpuScene::writeAttributes(uint32_t slot, const RenderProxy& p) {
    auto mesh = p.live ? meshFor(slot) : std::nullopt;
    const auto& range = proxyInstances[slot];
    for (uint32_t i = 0; i < range.count; ++i) {
        const auto index = range.first + i;
        static_cast<GpuInstance*>(instanceData.mapped)[index].info = {
            p.attributes.material, mesh ? meshes[*mesh].firstIndex : 0, p.attributes.entity, i};
        VkAccelerationStructureInstanceKHR a{};
        a.transform = rowMajor(shadowModel[index]);
        a.instanceCustomIndex = index;
        // Bit 0 is shadow visibility; the other bits retain material/reflective rays.
        a.mask = mesh && p.attributes.visible && bindings.rayPolicies[p.attributes.material].visible
                     ? (p.attributes.castShadow ? 0xffu : 0xfeu)
                     : 0u;
        a.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        if (p.live && bindings.rayPolicies[p.attributes.material].opaque)
            a.flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        a.accelerationStructureReference = mesh ? blas[*mesh].address : 0;
        static_cast<VkAccelerationStructureInstanceKHR*>(tlasInstances.mapped)[index] = a;
        statistics.attributes++;
    }
}

void GpuScene::writeSlot(uint32_t slot, const RenderProxy& p, bool zeroMotion) {
    writeTransform(slot, p, zeroMotion);
    writeAttributes(slot, p);
}

void GpuScene::reserveInstances(size_t count) {
    const auto current = instanceData.size / sizeof(GpuInstance);
    if (instanceData.handle && count <= current)
        return;
    // Shader storage range, TLAS primitive count and its 24-bit custom slot index
    // constrain representation; the scene itself has no fixed instance budget.
    const auto limit = std::min<VkDeviceSize>(
        {vk.properties.limits.maxStorageBufferRange / sizeof(GpuInstance),
         vk.asProperties.maxInstanceCount, VkDeviceSize(1) << 24});
    if (count > limit)
        throw std::runtime_error("Scene instance count " + std::to_string(count) +
                                 " exceeds device/index capacity " + std::to_string(limit));
    const auto capacity =
        std::min(limit, std::max<VkDeviceSize>(count, std::max(current * 2, VkDeviceSize(64))));
    // Renderer retires the previous GPU frame before applying the scene. Preserve
    // both mirrors byte-for-byte: growth must not force a resync or lose motion
    // history. Reading upload memory is confined to these infrequent reallocations.
    auto grow = [&](Buffer& buffer, VkDeviceSize stride, VkBufferUsageFlags usage) {
        auto next = vk.buffer(capacity * stride, usage, BufferMemory::Upload);
        std::memset(next.mapped, 0, size_t(capacity * stride));
        if (instanceCount)
            std::memcpy(next.mapped, buffer.mapped, size_t(instanceCount * stride));
        vk.destroy(buffer);
        buffer = next;
    };
    grow(instanceData, sizeof(GpuInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    grow(tlasInstances, sizeof(VkAccelerationStructureInstanceKHR),
         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    ++instanceCapacityGrowths;
    rebind();
}

void GpuScene::syncInstances(const Frame& frame) {
    // Proxy identity and GPU instance identity have independent lifetimes. Resizing
    // one batch must not move any other batch's data or temporal history.
    proxyInstances.resize(std::max(proxyInstances.size(), frame.proxies.size()));
    for (uint32_t slot = 0; slot < proxyInstances.size(); ++slot) {
        auto& range = proxyInstances[slot];
        const auto count =
            slot < frame.proxies.size() && frame.proxies[slot].live ? frame.proxies[slot].instanceCount : 0;
        if (range.count == count)
            continue;
        if (range.count) {
            std::memset(static_cast<VkAccelerationStructureInstanceKHR*>(tlasInstances.mapped) + range.first,
                        0, range.count * sizeof(VkAccelerationStructureInstanceKHR));
            instanceRanges.release(range.first, range.count);
        }
        range = {count ? instanceRanges.allocate(count) : 0, count, true};
    }
    reserveInstances(instanceRanges.size());
    instanceCount = instanceRanges.size();
    shadowModel.resize(instanceCount);
    motionFrame.resize(instanceCount, 0);
    // Retired tail indices no longer address upload memory after a later growth.
    movedLastFrame.erase(std::remove_if(movedLastFrame.begin(), movedLastFrame.end(),
                                        [this](uint32_t index) { return index >= instanceCount; }),
                         movedLastFrame.end());
}

// Brings the GPU mirror in line with a snapshot. The delta is applied when the mirror
// sits exactly at the revision the delta was built against; otherwise a snapshot was
// skipped and the only safe recovery is to rewrite every slot from the array, which
// costs exactly what the engine used to pay on every single frame.
void GpuScene::apply(const Frame& frame, bool reset) {
    if (!mirrorValid || frame.delta.topology != mirrorTopology)
        syncInstances(frame);
    const uint32_t capacity = uint32_t(frame.proxies.size());
    statistics = {};
    statistics.slots = capacity;
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
        if (!rayPoliciesDirty)
            for (auto slot : frame.delta.attributes)
                writeAttributes(slot, frame.proxies[slot]);
        // Structural changes rewrite both halves, so they run last and win. A slot
        // that just appeared, or was handed to a different object, has no previous
        // position worth interpolating from.
        for (auto slot : frame.delta.structural) {
            writeSlot(slot, frame.proxies[slot], true);
            statistics.structural++;
        }
        // Anything that moved on the previous frame and has now stopped would keep
        // reporting the motion it had then, smearing the denoiser behind it.
        for (auto slot : movedLastFrame)
            if (motionFrame[slot] != sceneStamp)
                settleMotion(slot);
    }
    // Material policy can change without an instance delta. Refresh ray masks and
    // opaque flags independently of temporal resets, including repeated snapshots.
    if (rayPoliciesDirty && !resync)
        for (uint32_t slot = 0; slot < capacity; ++slot)
            writeAttributes(slot, frame.proxies[slot]);
    rayPoliciesDirty = false;
    movedLastFrame = movedThisFrame;
    for (auto& range : proxyInstances)
        range.fresh = false;
    statistics.resynchronised = resync;
    mirrorRevision = frame.delta.revision;
    mirrorCapacity = capacity;
    mirrorValid = true;
    writes += statistics.transforms + statistics.attributes + statistics.settled;
    resyncs += resync;
    slotFrames += capacity;
    reserveTlas(frame);
}

void GpuScene::recordSkinnedBlas(VkCommandBuffer command) {
    for (auto m : dirtySkinMeshes)
        buildMeshAS(command, m, true);
    dirtySkinMeshes.clear();
}

// Decides this frame's top-level build and makes sure the structure it targets exists.
// Everything here is a CPU decision, so it happens while the graph is still being
// assembled; recordTlas only issues the build the decision produced.
void GpuScene::reserveTlas(const Frame& frame) {
    tlasGeometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    tlasGeometry.geometry.instances.data.deviceAddress = tlasInstances.address;
    tlasBuild = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    tlasBuild.geometryCount = 1;
    tlasBuild.pGeometries = &tlasGeometry;
    tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    // GPU ranges include inactive holes. Count/geometry changes rebuild; visibility
    // and transforms retain the same topology and can refit.
    uint32_t count = instanceCount;
    // Storage and scratch depend only on how many instances the structure can hold, so
    // reallocating is reserved for actually outgrowing it. Rebuilding reuses what is
    // already there.
    if (!tlas.handle || count > tlasCapacity) {
        // Share the instance arrays' growth policy instead of reallocating the
        // acceleration structure at every small increase in the scene's slot count.
        uint32_t capacity = uint32_t(instanceData.size / sizeof(GpuInstance));
        destroyAS(tlas);
        vk.destroy(tlasScratch);
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(vk.device,
                                                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &tlasBuild, &capacity, &sizes);
        tlas = createAS(tlasBuild.type, sizes.accelerationStructureSize);
        tlasScratch =
            vk.buffer(std::max(sizes.buildScratchSize, sizes.updateScratchSize) +
                          vk.asProperties.minAccelerationStructureScratchOffsetAlignment,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        tlasCapacity = capacity;
        tlasCount = 0;
        tlasBuilt = false;
        rebind();
    }
    // Refitting cannot absorb a slot appearing or rebinding to different geometry.
    // That arrives as a running count rather than a flag, so it is still detected when
    // the snapshot carrying it was skipped, and no instance reference ever changes
    // underneath an incremental update.
    tlasUpdate = tlasBuilt && tlasCount == count && frame.delta.topology == mirrorTopology;
    tlasBuilt = true;
    tlasCount = count;
    mirrorTopology = frame.delta.topology;
    statistics.tlasRebuilt = !tlasUpdate;
    tlasRebuilds += !tlasUpdate;
    tlasBuild.mode = tlasUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuild.srcAccelerationStructure = tlasUpdate ? tlas.handle : VK_NULL_HANDLE;
    tlasBuild.dstAccelerationStructure = tlas.handle;
    tlasBuild.scratchData.deviceAddress = alignedScratch(tlasScratch);
}

void GpuScene::recordTlas(VkCommandBuffer c, GpuProfiler& profiler) {
    VkAccelerationStructureBuildRangeInfoKHR range{tlasCount, 0, 0, 0};
    const auto* rangePointer = &range;
    GpuScope scope(profiler, c, tlasUpdate ? "TLAS Refit" : "TLAS Build");
    vkCmdBuildAccelerationStructuresKHR(c, 1, &tlasBuild, &rangePointer);
}

void GpuScene::beginDraws() {
    drawSlots.clear();
}

std::vector<RasterBatch> GpuScene::prepareDraws(std::vector<RasterDraw> draws) {
    // Exactly one sortable item per proxy, independent of its instance count.
    std::stable_sort(draws.begin(), draws.end(),
                     [](const RasterDraw& a, const RasterDraw& b) { return a.sortKey < b.sortKey; });
    std::vector<RasterBatch> batches;
    bool previousMergeable = false;
    const auto identityCapacity = uint32_t(instanceData.size / sizeof(GpuInstance));
    for (const auto& draw : draws) {
        auto geometry = meshFor(draw.slot);
        if (!geometry)
            continue;
        const auto& mesh = meshes[*geometry];
        const auto& instances = proxyInstances[draw.slot];
        const bool mergeable = instances.count == 1;
        if (!mergeable) {
            // A contiguous authored group addresses the persistent identity prefix
            // directly. Even a million instances produce one item and one range,
            // with no per-frame index expansion or per-instance sorting.
            batches.push_back(
                {mesh.firstIndex, mesh.indexCount, instances.first, instances.count, draw.pipeline});
            previousMergeable = false;
            continue;
        }
        // Only neighbours in the sorted stream may merge. Material values remain
        // per slot; shared geometry and the complete pipeline determine compatibility.
        if (previousMergeable && !batches.empty() && batches.back().pipeline == draw.pipeline &&
            batches.back().firstIndex == mesh.firstIndex && batches.back().indexCount == mesh.indexCount)
            ++batches.back().instanceCount;
        else
            batches.push_back({mesh.firstIndex, mesh.indexCount,
                               identityCapacity + uint32_t(drawSlots.size()), 1, draw.pipeline});
        drawSlots.push_back(instances.first);
        previousMergeable = true;
    }
    return batches;
}

void GpuScene::uploadDraws() {
    // Explicit groups reuse [0, identityCapacity). Dynamic singleton batches use
    // a transient indirection suffix. Both feed the same instance-rate vertex input.
    const auto identityCapacity = uint32_t(instanceData.size / sizeof(GpuInstance));
    const VkDeviceSize bytes = (VkDeviceSize(identityCapacity) + drawSlots.size()) * sizeof(uint32_t);
    if (bytes > drawInstances.size) {
        const auto capacity = std::max(bytes, drawInstances.size * 2);
        vk.destroy(drawInstances);
        drawInstances = vk.buffer(capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, BufferMemory::Upload);
        drawIdentityCapacity = 0;
        rebind();
    }
    auto* slots = static_cast<uint32_t*>(drawInstances.mapped);
    for (uint32_t i = drawIdentityCapacity; i < identityCapacity; ++i)
        slots[i] = i;
    drawIdentityCapacity = identityCapacity;
    if (!drawSlots.empty())
        std::memcpy(slots + identityCapacity, drawSlots.data(), drawSlots.size() * sizeof(uint32_t));
}

void GpuScene::recordDraws(VkCommandBuffer c, const std::vector<RasterBatch>& draws) {
    const VkBuffer buffers[] = {vertexData.handle, drawInstances.handle};
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(c, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(c, indexData.handle, 0, VK_INDEX_TYPE_UINT32);
    VkPipeline bound = VK_NULL_HANDLE;
    for (const auto& draw : draws) {
        if (draw.pipeline != bound) {
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
            bound = draw.pipeline;
        }
        vkCmdDrawIndexed(c, draw.indexCount, draw.instanceCount, draw.firstIndex, 0, draw.firstInstance);
        ++statistics.rasterDrawCalls;
        statistics.rasterInstances += draw.instanceCount;
        statistics.largestRasterBatch = std::max(statistics.largestRasterBatch, draw.instanceCount);
    }
}
} // namespace whimsical
