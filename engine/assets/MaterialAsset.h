#pragma once
#include "StaticMesh.h"
#include "Material.h"

namespace whimsical {
struct MaterialAsset final : Asset {
    Material parameters;
};
} // namespace whimsical
