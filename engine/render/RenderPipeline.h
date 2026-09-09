#pragma once
#include "GpuScene.h"
#include "MaterialBindings.h"
#include "RenderResources.h"
#include "ShaderCompiler.h"
#include "renderCore/graph/RenderGraph.h"
#include "renderCore/RenderCore.h"
#include <array>
#include <map>

namespace whimsical {
class NrdDenoiser;
class UiRenderer;
class GpuRenderTargets;

// What the frame needs from outside the pipeline. Everything else a pass touches it
// declares to the graph.
struct FrameSetup {
    const Frame* frame = nullptr;
    GpuScene* scene = nullptr;
    NrdDenoiser* denoiser = nullptr;
    UiRenderer* ui = nullptr;
    GpuProfiler* profiler = nullptr;
    GpuRenderTargets* targets = nullptr;
    uint64_t frameNumber = 0;
    float frameMs = 16.7f;
    bool reset = false;        // temporal accumulation restarts this frame
    bool clearHistory = false; // history storage itself is undefined and must be zeroed
    bool audit = false;
    bool capture = false;
    ViewRect viewport;
    rg::ResourceList auditSignals;
};

// Owns the programs and declares the frame. This is the only file that knows which pass
// exists and in what order; what each one reads and writes comes from the compiled shader
// it runs, and the graph turns both into dependencies, barriers, layouts and storage reuse.
class RenderPipeline {
  public:
    RenderPipeline(VulkanContext&, ShaderCompiler&, rc::GraphContext&, const RenderResources&);
    ~RenderPipeline();
    void ensurePrograms(const std::vector<Material>&, const ShaderCompiler::ShaderSet&);
    size_t rasterProgramCount() const {
        return rasterPrograms_.size();
    }
    void build(rg::RenderGraph&, const FrameSetup&);

  private:
    // The passes linked against the live Shader set come first, in the order
    // ShaderCompiler::surfacePasses names them; the rest have no material in them.
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
    using SurfacePrograms = std::array<const rg::Program*, ShaderCompiler::surfacePasses.size()>;
    static constexpr std::array screenSpacePasses = {"composite.comp", "di_confidence.comp",
                                                     "di_gradient_filter.comp"};
    static_assert(Composite == ShaderCompiler::surfacePasses.size() &&
                  Composite + screenSpacePasses.size() == PassCount);
    VulkanContext& vk_;
    ShaderCompiler& shaders_;
    rc::GraphContext& execution_;
    rg::ResourcePool& pool_;
    const RenderResources& r_;
    // The programs the frame declares itself with. The screen-space ones exist for the
    // whole run; the surface ones are relinked whenever the live Shader set changes, so a
    // pass points at whichever program is current rather than owning it.
    std::array<const rg::Program*, PassCount> compute_{};
    std::map<RasterKey, VkPipeline> rasterPrograms_;
    std::map<RasterKey, VkPipeline> entityIDPrograms_;
    std::map<ShaderCompiler::ShaderSet, SurfacePrograms> computePrograms_;
    // The G-buffer pass binds one pipeline per material, so what it touches is the union
    // over them plus the vertex stage they share.
    std::vector<rg::ShaderAccess> rasterAccess_;
    std::vector<rg::ShaderAccess> entityIDAccess_;
    std::vector<rg::ShaderAccess> displayAccess_;

    VkPipeline createRaster(const RasterKey&, bool entityID = false);
};
} // namespace whimsical
