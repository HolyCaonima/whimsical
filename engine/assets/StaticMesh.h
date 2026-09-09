#pragma once
#include "Asset.h"
#include "core/Math.h"
#include <vector>

namespace whimsical {
struct StaticVertex {
    vec3 position, normal;
    vec2 uv;
    vec4 tangent;
    vec3 color{1};
};
struct StaticMesh final : Asset {
    std::vector<StaticVertex> vertices;
    std::vector<uint32_t> indices;
    static std::shared_ptr<StaticMesh> decode(const std::string&);
};
struct TextureAsset final : Asset {
    uint32_t width = 0, height = 0;
    bool srgb = false;
    std::vector<uint32_t> pixels;
    static std::shared_ptr<TextureAsset> decode(const std::string&, const Json& metadata);
};
} // namespace whimsical
