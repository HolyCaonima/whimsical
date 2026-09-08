#include "RenderResources.h"
#include <Rtxdi/RtxdiParameters.h>

namespace afterlight {
using namespace rg;

namespace {
uint64_t pixels(uint32_t width, uint32_t height) {
    return uint64_t(width) * height;
}

Declaration image(const char* name, Format format, Lifetime lifetime, const char* shaderName,
                  const char* previousName = nullptr) {
    Declaration d;
    d.name = name;
    d.kind = Kind::Image;
    d.lifetime = lifetime;
    d.format = format;
    if (shaderName)
        d.view = {BindingType::StorageImage, false, shaderName};
    if (previousName)
        d.previous = {BindingType::StorageImage, false, previousName};
    return d;
}

Declaration storage(const char* name, Lifetime lifetime, const char* block, const char* body,
                    std::function<uint64_t(uint32_t, uint32_t)> bytes, bool readOnly = false) {
    Declaration d;
    d.name = name;
    d.kind = Kind::Buffer;
    d.lifetime = lifetime;
    d.bytes = std::move(bytes);
    d.view = {BindingType::Storage, readOnly, {}, block, body};
    return d;
}

Declaration inShared(Declaration d) {
    d.section = Section::Shared;
    return d;
}
Declaration inRtxdi(Declaration d) {
    d.section = Section::Rtxdi;
    return d;
}
} // namespace

SceneResources::SceneResources(Registry& registry) {
    Declaration globalBlock;
    globalBlock.name = "globals";
    globalBlock.kind = Kind::Buffer;
    globalBlock.lifetime = Lifetime::External;
    globalBlock.view = {BindingType::Uniform, true, "g", "Globals", R"(    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 view;
    mat4 inverseViewProjection;
    vec4 eyeTime;
    vec4 resolution;
    vec4 player;
    vec4 destination;
    vec4 renderSettings;
    vec4 previousEye;
    uvec4 counts;)"};
    globals = registry.declare(inShared(std::move(globalBlock)));
    instances = registry.declare(inShared(storage("instances", Lifetime::External, "Instances",
                                                  "    Instance instances[];", nullptr, true)));
    materials = registry.declare(inShared(storage("materials", Lifetime::External, "Materials",
                                                  "    Material materials[];", nullptr, true)));
    lights = registry.declare(
        inShared(storage("lights", Lifetime::External, "Lights", "    Light lights[];", nullptr, true)));

    Declaration textureArray;
    textureArray.name = "materialTextures";
    textureArray.lifetime = Lifetime::External;
    textureArray.view = {BindingType::SamplerArray, true, "materialTextures"};
    textureArray.view.count = MaxTextures;
    textures = registry.declare(inShared(std::move(textureArray)));

    vertices = registry.declare(
        storage("vertices", Lifetime::External, "Vertices", "    Vertex vertices[];", nullptr, true));
    indices = registry.declare(
        storage("indices", Lifetime::External, "Indices", "    uint indices[];", nullptr, true));

    Declaration structure;
    structure.name = "tlas";
    structure.kind = Kind::Tlas;
    structure.lifetime = Lifetime::Imported;
    structure.view = {BindingType::Tlas, true, "scene"};
    tlas = registry.declare(std::move(structure));
}

ResourceList SceneResources::shared() const {
    return {globals, instances, materials, lights, textures};
}
ResourceList SceneResources::geometry() const {
    return {vertices, indices};
}

GBufferResources::GBufferResources(Registry& registry) {
    albedo = registry.declare(
        image("gAlbedo", Format::RGBA16F, Lifetime::History, "gAlbedo", "previousAlbedo"));
    normal = registry.declare(
        image("gNormal", Format::RGBA16F, Lifetime::History, "gNormal", "previousNormal"));
    position = registry.declare(
        image("gPosition", Format::RGBA32F, Lifetime::History, "gPosition", "previousPosition"));
    motion = registry.declare(image("gMotion", Format::RGBA16F, Lifetime::Transient, "gMotion"));
    viewZ =
        registry.declare(image("gViewZ", Format::R32F, Lifetime::History, "gViewZ", "previousViewZ"));
    emission = registry.declare(image("gEmission", Format::RGBA16F, Lifetime::Transient, "gEmission"));
    depth = registry.declare(image("gDepth", Format::D32, Lifetime::Transient, nullptr));
}

ResourceList GBufferResources::attachments() const {
    return {albedo, normal, position, motion, viewZ, emission};
}
ResourceList GBufferResources::surface() const {
    return {albedo, normal, position, viewZ, emission};
}
ResourceList GBufferResources::previousSurface() const {
    return {previous(albedo), previous(normal), previous(position), previous(viewZ)};
}

RestirDiResources::RestirDiResources(Registry& registry) {
    // Four block-linear arrays. They rotate in pairs across frames rather than being
    // copied, so the buffer as a whole carries its own history: Persistent, not History.
    reservoirs = registry.declare(inRtxdi(storage(
        "diReservoirs", Lifetime::Persistent, "DiReservoirs", "    RTXDI_PackedDIReservoir diReservoirs[];",
        [](uint32_t w, uint32_t h) {
            constexpr uint32_t block = RTXDI_RESERVOIR_BLOCK_SIZE;
            return uint64_t((w + block - 1) / block) * ((h + block - 1) / block) * block * block *
                   sizeof(RTXDI_PackedDIReservoir) * 4;
        })));
    neighbours = registry.declare(inRtxdi(storage("diNeighbors", Lifetime::External, "DiNeighbors",
                                                  "    vec2 diNeighbors[];", nullptr, true)));
    lightSamples = registry.declare(inRtxdi(
        storage("diLights", Lifetime::External, "DiLights",
                "    vec4 lightDistribution[256]; // CDF, discrete PDF, unused, unused\n"
                "    Light previousLights[256];",
                nullptr, true)));

    // The gradient runs on a third-resolution stratum grid.
    Declaration strata = image("diGradient", Format::RGBA16F, Lifetime::Transient, "diGradient");
    strata.divisor = GradientDivisor;
    gradient = registry.declare(strata);
    strata.name = strata.view.name = "filteredDiGradient";
    filteredGradient = registry.declare(strata);

    diffuseConfidence = registry.declare(
        image("diffuseConfidence", Format::R16F, Lifetime::Transient, "diffuseConfidence"));
    specularConfidence = registry.declare(
        image("specularConfidence", Format::R16F, Lifetime::Transient, "specularConfidence"));
    luminance = registry.declare(
        image("diLuminance", Format::RGBA16F, Lifetime::History, "diLuminance", "previousDiLuminance"));
    confidenceHistory = registry.declare(
        image("diConfidenceHistory", Format::RG16F, Lifetime::Persistent, "diConfidenceHistory"));
}

ResourceList RestirDiResources::sdk() const {
    return {reservoirs, neighbours, lightSamples};
}

RestirGiResources::RestirGiResources(Registry& registry) {
    Declaration pair = storage("giReservoirs", Lifetime::History, "CurrentGI",
                               "    GIReservoir currentGI[];",
                               [](uint32_t w, uint32_t h) { return pixels(w, h) * 64; });
    pair.previous = {BindingType::Storage, true, {}, "PreviousGI", "    GIReservoir previousGI[];"};
    reservoirs = registry.declare(std::move(pair));
    candidate = registry.declare(storage("giCandidate", Lifetime::Transient, "CandidateGI",
                                         "    GIReservoir candidateGI[];",
                                         [](uint32_t w, uint32_t h) { return pixels(w, h) * 64; }));
}

ShadingResources::ShadingResources(Registry& registry) {
    rawDiffuse =
        registry.declare(image("rawDiffuse", Format::RGBA16F, Lifetime::Transient, "rawDiffuse"));
    rawSpecular =
        registry.declare(image("rawSpecular", Format::RGBA16F, Lifetime::Transient, "rawSpecular"));
    denoisedDiffuse = registry.declare(
        image("denoisedDiffuse", Format::RGBA16F, Lifetime::Transient, "denoisedDiffuse"));
    denoisedSpecular = registry.declare(
        image("denoisedSpecular", Format::RGBA16F, Lifetime::Transient, "denoisedSpecular"));
    directDebug =
        registry.declare(image("directDebug", Format::RGBA16F, Lifetime::Transient, "directDebug"));
    indirectDebug =
        registry.declare(image("indirectDebug", Format::RGBA16F, Lifetime::Transient, "indirectDebug"));
    hud = registry.declare(image("hudImage", Format::RGBA8, Lifetime::Transient, "hudImage"));
    display =
        registry.declare(image("finalImage", Format::RGBA16F, Lifetime::Transient, "finalImage"));
}

// Nothing declared here has a shader view, which is what lets an optional resource exist on
// one run and not the next without moving a single binding number.
OutputResources::OutputResources(Registry& registry, bool captureEnabled, uint32_t auditSignals) {
    Declaration presented;
    presented.name = "swapchain";
    presented.lifetime = Lifetime::Imported;
    presented.format = Format::RGBA8;
    swapchain = registry.declare(std::move(presented));

    auto readback = [&](const char* name, std::function<uint64_t(uint32_t, uint32_t)> bytes) {
        Declaration d;
        d.name = name;
        d.kind = Kind::Buffer;
        d.lifetime = Lifetime::Persistent;
        d.readback = true;
        d.bytes = std::move(bytes);
        return registry.declare(std::move(d));
    };
    if (captureEnabled) {
        capture = registry.declare(image("capture", Format::RGBA8, Lifetime::Transient, nullptr));
        screenshot = readback("screenshot", [](uint32_t w, uint32_t h) { return pixels(w, h) * 4; });
    }
    if (auditSignals)
        audit = readback("audit", [auditSignals](uint32_t w, uint32_t h) {
            return pixels(w, h) * 8 * auditSignals;
        });
}

RenderResources::RenderResources(bool captureEnabled, uint32_t auditSignals)
    : scene(registry), gbuffer(registry), di(registry), gi(registry), shading(registry),
      output(registry, captureEnabled, auditSignals) {}
} // namespace afterlight
