#pragma once
#include "Asset.h"
#include "core/Math.h"

namespace whimsical {
enum class PropertyType { Float = 1, Vec2, Vec3, Vec4 };
enum class MaterialModel { MetallicRoughness };
enum class SurfaceMode { Opaque, Masked };
enum class SurfaceCull { None, Back, Front };
struct ShaderProperty {
    std::string name;
    PropertyType type;
    vec4 value{0};
};
struct ShaderRenderState {
    SurfaceMode mode = SurfaceMode::Opaque;
    SurfaceCull cull = SurfaceCull::None;
    float alphaCutoff = .5f;
};
// Source is the body of EvaluateSurface(MaterialContext ctx). The compiler supplies
// typed `properties` and texture handles in `textures` from this ordered schema.
struct ShaderAsset final : Asset {
    static constexpr uint32_t MaxProperties = 8, MaxTextures = 8;
    std::string source;
    std::vector<ShaderProperty> properties;
    std::vector<std::string> textures;
    MaterialModel model = MaterialModel::MetallicRoughness;
    ShaderRenderState renderState;
    static std::shared_ptr<ShaderAsset> decode(const Json& metadata, const std::string& source);
};
} // namespace whimsical
