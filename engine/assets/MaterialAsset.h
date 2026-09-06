#pragma once
#include "StaticMesh.h"
#include "Material.h"

namespace afterlight {
struct MaterialAsset final : Asset {
    Material parameters;
};
} // namespace afterlight
