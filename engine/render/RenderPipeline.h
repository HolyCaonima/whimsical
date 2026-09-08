#pragma once
#include "GpuScene.h"
#include "MaterialBindings.h"
#include "RenderResources.h"
#include "ShaderCompiler.h"
#include "graph/RenderGraph.h"
#include <array>
#include <map>

namespace afterlight {
class NrdDenoiser;
class UiRenderer;

// What the frame needs from outside the pipeline. Everything else a pass touches it
// declares to the graph.
struct FrameSetup {
    const Frame* frame = nullptr;
    GpuScene* scene = nullptr;
    NrdDenoiser* denoiser = nullptr;
    UiRenderer* ui = nullptr;
    GpuProfiler* profiler = nullptr;
    uint64_t frameNumber = 0;
    float frameMs = 16.7f;
    bool reset = false;        // temporal accumulation restarts this frame
    bool clearHistory = false; // history storage itself is undefined and must be zeroed
    bool audit = false;
    bool capture = false;
    rg::ResourceList auditSignals;
};

// Owns the programs and declares the frame. This is the only file that knows which pass
// exists, in what order, and what each one reads and writes; the graph turns that into
// dependencies, barriers, layouts and storage reuse.
class RenderPipeline {
  public:
    RenderPipeline(VulkanContext&, ShaderCompiler&, rg::ResourcePool&, const RenderResources&);
    ~RenderPipeline();
    void ensurePrograms(const MaterialBindings&);
    const std::map<std::shared_ptr<const ShaderAsset>, VkPipeline>& rasterPrograms() const {
        return rasterPrograms_;
    }
    size_t rasterProgramCount() const {
        return rasterPrograms_.size();
    }
    void build(rg::RenderGraph&, const FrameSetup&);

  private:
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
    VulkanContext& vk_;
    ShaderCompiler& shaders_;
    rg::ResourcePool& pool_;
    const RenderResources& r_;
    std::array<VkPipeline, PassCount> compute_{};
    std::map<std::shared_ptr<const ShaderAsset>, VkPipeline> rasterPrograms_;
    std::map<ShaderCompiler::ShaderSet, std::array<VkPipeline, ShaderCompiler::surfacePasses.size()>>
        computePrograms_;

    VkPipeline createCompute(VkShaderModule);
    VkPipeline createRaster(const std::shared_ptr<const ShaderAsset>&);
    // Bound by every ray-query pass: the scene it traces and the surfaces it shades.
    void traceInputs(rg::RenderGraph::Builder&) const;
};
} // namespace afterlight
