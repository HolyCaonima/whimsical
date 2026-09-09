#pragma once
#include "ShaderAsset.h"
#include <tuple>

namespace whimsical {
class AssetManager;
struct TextureAsset;
enum class SurfaceMode { Opaque, Masked };
enum class SurfaceCull { None, Back, Front };
// Surface writes the deferred lighting inputs. Display writes unlit albedo + emission
// after tone mapping, in display colour space. This is an output contract, not an entity type.
enum class MaterialDomain { Surface, Display };
enum class DepthCompare { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class MaterialBlend { Opaque, Alpha, Additive };
struct MaterialRenderState {
    MaterialDomain domain = MaterialDomain::Surface;
    SurfaceMode surface = SurfaceMode::Opaque;
    SurfaceCull cull = SurfaceCull::None;
    bool depthTest = true, depthWrite = true;
    DepthCompare depthCompare = DepthCompare::Less;
    MaterialBlend blend = MaterialBlend::Opaque;
    float alphaCutoff = .5f;
    bool rayVisible = true;
    // Layer 0 shares scene depth. Display layers > 0 run in ascending order, each
    // starting with independent depth; all draws in one layer share that depth.
    uint32_t layer = 0;
    auto values() const {
        return std::tie(domain, surface, cull, depthTest, depthWrite, depthCompare, blend, alphaCutoff,
                        rayVisible, layer);
    }
    bool operator==(const MaterialRenderState& other) const {
        return values() == other.values();
    }
    Json json() const;
    static MaterialRenderState fromJson(const Json&);
};
// CPU instances contain no descriptor, pipeline or GPU table indices.
struct Material {
    std::shared_ptr<const ShaderAsset> shader;
    std::vector<vec4> properties;
    std::vector<std::shared_ptr<const TextureAsset>> textures;
    MaterialRenderState renderState;
    bool operator==(const Material& other) const {
        return shader == other.shader && properties == other.properties && textures == other.textures &&
               renderState == other.renderState;
    }
};
// Material asset payload. Resolution is an
// asset-loading operation, independent of the renderer and its binding tables.
struct MaterialDefinition {
    AssetRef shader;
    Json properties = Json::object();
    std::map<std::string, AssetRef> textures;
    MaterialRenderState renderState;
    Json json() const;
    static MaterialDefinition fromJson(const Json&);
    Material resolve(AssetManager&) const;
    static MaterialDefinition capture(const Material&, const AssetManager&);
};
} // namespace whimsical
