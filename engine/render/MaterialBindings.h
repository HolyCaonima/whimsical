#pragma once
#include "assets/Material.h"
#include <array>
#include "ShaderCompiler.h"

namespace afterlight {
struct alignas(16) GpuMaterial {
    glm::uvec4 info{0};
    std::array<vec4, ShaderAsset::MaxProperties> properties{};
    std::array<glm::ivec4, ShaderAsset::MaxTextures / 4> textures{glm::ivec4(-1), glm::ivec4(-1)};
};
static_assert(sizeof(GpuMaterial) == 176, "Surface ABI mismatch");
// Rebuilt only when CPU material instances change. All transient indices live here.
struct MaterialBindings {
    ShaderCompiler::ShaderSet shaders;
    std::vector<std::shared_ptr<const TextureAsset>> textures;
    std::vector<GpuMaterial> materials;
    static MaterialBindings build(const std::vector<Material>&);
};
} // namespace afterlight
