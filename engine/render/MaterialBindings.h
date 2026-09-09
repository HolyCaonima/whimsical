#pragma once
#include "assets/Material.h"
#include <array>
#include "ShaderCompiler.h"

namespace whimsical {
struct alignas(16) GpuMaterial {
    // x: shader, y: surface/cull flags, z: alpha cutoff bits, w: reserved.
    glm::uvec4 info{0};
    std::array<vec4, ShaderAsset::MaxProperties> properties{};
    std::array<glm::ivec4, ShaderAsset::MaxTextures / 4> textures{glm::ivec4(-1), glm::ivec4(-1)};
};
static_assert(sizeof(GpuMaterial) == 176, "Surface ABI mismatch");
// Only the resolved properties that affect acceleration-structure instances cross
// into GpuScene. Shader values and raster scheduling do not affect this policy.
struct RayMaterialPolicy {
    bool visible = true, opaque = true;
    bool operator==(const RayMaterialPolicy& other) const {
        return visible == other.visible && opaque == other.opaque;
    }
};
// Rebuilt only when CPU material instances change. All transient indices live here.
struct MaterialBindings {
    ShaderCompiler::ShaderSet shaders;
    std::vector<std::shared_ptr<const TextureAsset>> textures;
    std::vector<GpuMaterial> materials;
    std::vector<RayMaterialPolicy> rayPolicies; // CPU-only, indexed by material slot.
    static MaterialBindings build(const std::vector<Material>&);
};
} // namespace whimsical
