#pragma once
#include "ShaderAsset.h"

namespace whimsical {
class AssetManager;
struct TextureAsset;
// CPU instances contain no descriptor, pipeline or GPU table indices.
struct Material {
    std::shared_ptr<const ShaderAsset> shader;
    std::vector<vec4> properties;
    std::vector<std::shared_ptr<const TextureAsset>> textures;
    bool operator==(const Material& other) const {
        return shader == other.shader && properties == other.properties && textures == other.textures;
    }
};
// Material asset payload. Resolution is an
// asset-loading operation, independent of the renderer and its binding tables.
struct MaterialDefinition {
    AssetRef shader;
    Json properties = Json::object();
    std::map<std::string, AssetRef> textures;
    Json json() const;
    static MaterialDefinition fromJson(const Json&);
    Material resolve(AssetManager&) const;
    static MaterialDefinition capture(const Material&, const AssetManager&);
};
} // namespace whimsical
