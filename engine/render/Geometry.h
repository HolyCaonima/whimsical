#pragma once
#include "core/Types.h"
namespace afterlight {
struct GpuVertex {
    vec4 position;
    vec4 normal;
    vec4 color{1};
    vec4 previousPosition{0};
    vec4 uv{0};
    vec4 tangent{1, 0, 0, 1};
};
struct MeshRange {
    uint32_t firstIndex = 0, indexCount = 0;
};
} // namespace afterlight
