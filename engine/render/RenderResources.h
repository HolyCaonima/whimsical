#pragma once
#include "core/Types.h"
#include "render/graph/Registry.h"

// Every logical resource of the lighting pipeline, grouped by the feature that owns it.
// A declaration states what a resource *is* — format, screen fraction, who owns the memory,
// how long the contents mean anything, and how a shader names it. Binding numbers,
// descriptor writes, allocation, clearing, barriers and the GLSL declaration block are all
// derived from it, so adding a resource is one line in the feature that needs it.
namespace whimsical {
constexpr uint32_t MaxInstances = 1024, MaxLights = 256, MaxMaterials = 256, MaxTextures = 64;
// The DI gradient is measured on strata of this many pixels a side.
constexpr uint16_t GradientDivisor = 3;
// The four DI reservoir arrays swap in pairs each frame instead of being copied, so a
// shader asks for a role and gets the array currently playing it.
constexpr uint32_t DiLayerRotation = 2;

struct alignas(16) GpuGlobals {
    mat4 vp, previousVp, view, inverseVp;
    vec4 eyeTime, resolution, renderSettings, previousEye;
    glm::uvec4 counts;
};

// Scene inputs. The scene owns these allocations because they grow with the content rather
// than with the screen, so they are External: the graph binds them and never allocates or
// synchronises them. Both levels of the acceleration structure are Imported instead — the
// scene allocates them, the graph orders the refits, the top-level build and the ray
// queries against each other. The bottom level has no descriptor of its own, so the
// top-level declaration is what reaches it; buildInstances has no descriptor either, and
// is what a top-level build actually reads.
struct SceneResources {
    rg::ResourceId globals, outlines, instances, materials, lights, vertices, indices, textures, buildInstances, blas,
        tlas;
    explicit SceneResources(rg::Registry&);
    rg::ResourceList geometry() const; // vertex/index buffers, as a structure build reads them
};

// The four halves that the next frame reads back are History; the graph gives each of them
// two allocations and swaps which one the "previous" name resolves to. No copy exists.
struct GBufferResources {
    rg::ResourceId albedo, normal, position, motion, viewZ, emission, depth;
    explicit GBufferResources(rg::Registry&);
};

struct RestirDiResources {
    rg::ResourceId reservoirs, neighbours, lightSamples, gradient, filteredGradient, diffuseConfidence,
        specularConfidence, luminance, confidenceHistory;
    explicit RestirDiResources(rg::Registry&);
};

struct RestirGiResources {
    rg::ResourceId reservoirs, candidate;
    explicit RestirGiResources(rg::Registry&);
};

struct ShadingResources {
    rg::ResourceId rawDiffuse, rawSpecular, denoisedDiffuse, denoisedSpecular, directDebug, indirectDebug,
        display;
    explicit ShadingResources(rg::Registry&);
};

// Frame output. The swapchain image is Imported every frame; the readback buffers only
// exist when the run actually asks for them, so an ordinary run never pays for them.
struct OutputResources {
    rg::ResourceId swapchain, presentation, screenshot, audit;
    OutputResources(rg::Registry&, bool captureEnabled, uint32_t auditSignals);
};

struct RenderResources {
    rg::Registry registry;
    SceneResources scene;
    GBufferResources gbuffer;
    RestirDiResources di;
    RestirGiResources gi;
    ShadingResources shading;
    OutputResources output;
    RenderResources(bool captureEnabled, uint32_t auditSignals);
};
} // namespace whimsical
