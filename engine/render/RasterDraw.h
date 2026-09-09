#pragma once
#include "assets/Material.h"
#include "renderCore/vulkan/VulkanContext.h"

namespace whimsical {
// Pipeline identity contains only fixed-function state and the output contract.
// Material values, clipping, ray visibility and scheduling layers do not compile variants.
struct RasterKey {
    std::shared_ptr<const ShaderAsset> shader;
    MaterialDomain domain;
    SurfaceCull cull;
    bool depthTest, depthWrite;
    DepthCompare depthCompare;
    MaterialBlend blend;
    static RasterKey from(const Material& m) {
        const auto& s = m.renderState;
        return {m.shader, s.domain, s.cull, s.depthTest, s.depthWrite, s.depthCompare, s.blend};
    }
    bool operator<(const RasterKey& o) const {
        return std::tie(shader, domain, cull, depthTest, depthWrite, depthCompare, blend) <
               std::tie(o.shader, o.domain, o.cull, o.depthTest, o.depthWrite, o.depthCompare, o.blend);
    }
};
struct RasterDraw {
    uint32_t slot;
    VkPipeline pipeline;
};
} // namespace whimsical
