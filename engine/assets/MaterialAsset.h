#pragma once
#include "StaticMesh.h"
#include "core/Types.h"

namespace afterlight {
struct MaterialAsset final : Asset {
    Material parameters;
    // Base color, tangent-space normal, and occlusion/roughness/metallic.
    std::array<std::shared_ptr<const TextureAsset>, 3> textures;
};
} // namespace afterlight
