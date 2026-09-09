#include "MaterialBindings.h"
#include <algorithm>
#include <functional>

namespace whimsical {
MaterialBindings MaterialBindings::build(const std::vector<Material>& instances) {
    MaterialBindings result;
    for (const auto& material : instances) {
        if (!material.shader || material.properties.size() != material.shader->properties.size() ||
            material.textures.size() != material.shader->textures.size())
            throw std::invalid_argument("Material must match a resolved Shader schema");
        if (std::find(result.shaders.begin(), result.shaders.end(), material.shader) == result.shaders.end())
            result.shaders.push_back(material.shader);
    }
    // A Material reorder must not change ray dispatch IDs or trigger relinking.
    std::sort(result.shaders.begin(), result.shaders.end(), [](const auto& a, const auto& b) {
        if (a->header().id != b->header().id)
            return a->header().id < b->header().id;
        // Frames can retain two immutable generations of the same asset after reload.
        return std::less<const ShaderAsset*>{}(a.get(), b.get());
    });
    for (const auto& material : instances) {
        GpuMaterial gpu;
        gpu.info.x = uint32_t(std::find(result.shaders.begin(), result.shaders.end(), material.shader) -
                              result.shaders.begin());
        std::copy(material.properties.begin(), material.properties.end(), gpu.properties.begin());
        for (size_t t = 0; t < material.textures.size(); ++t) {
            if (!material.textures[t])
                continue;
            auto found = std::find(result.textures.begin(), result.textures.end(), material.textures[t]);
            gpu.textures[t / 4][int(t % 4)] = int(found - result.textures.begin());
            if (found == result.textures.end())
                result.textures.push_back(material.textures[t]);
        }
        result.materials.push_back(gpu);
    }
    return result;
}
} // namespace whimsical
