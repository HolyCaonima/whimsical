#pragma once
#include "Asset.h"
#include "core/Math.h"

namespace whimsical {
enum class PropertyType { Float = 1, Vec2, Vec3, Vec4 };
enum class MaterialModel { MetallicRoughness };
struct ShaderProperty {
    std::string name;
    PropertyType type;
    vec4 value{0};
};
// Source is the body of EvaluateSurface(MaterialContext ctx). The compiler supplies
// typed `properties` and texture handles in `textures` from this ordered schema.
struct ShaderAsset final : Asset {
    static constexpr uint32_t MaxProperties = 8, MaxTextures = 8;
    std::string source;
    std::vector<ShaderProperty> properties;
    std::vector<std::string> textures;
    MaterialModel model = MaterialModel::MetallicRoughness;
    static std::shared_ptr<ShaderAsset> decode(const Json& metadata, const std::string& source);
};
} // namespace whimsical
