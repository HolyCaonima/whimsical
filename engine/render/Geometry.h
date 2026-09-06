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
inline std::array<MeshRange, 2> buildPrimitives(std::vector<GpuVertex>& vertices,
                                                std::vector<uint32_t>& indices) {
    std::array<MeshRange, 2> ranges;
    const vec3 normals[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto n : normals) {
        vec3 tangent = std::abs(n.y) > .5f ? vec3(1, 0, 0) : glm::normalize(glm::cross(vec3(0, 1, 0), n)),
             bitangent = glm::cross(n, tangent);
        uint32_t base = uint32_t(vertices.size());
        for (auto uv : {vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, 1)})
            vertices.push_back({vec4(n * .5f + (tangent * uv.x + bitangent * uv.y) * .5f, 1), vec4(n, 0)});
        for (auto i : {0u, 1u, 2u, 0u, 2u, 3u})
            indices.push_back(base + i);
    }
    ranges[0] = {0, uint32_t(indices.size())};
    ranges[1].firstIndex = uint32_t(indices.size());
    constexpr uint32_t segments = 24, rings = 8;
    uint32_t base = uint32_t(vertices.size());
    for (uint32_t hemisphere = 0; hemisphere < 2; hemisphere++)
        for (uint32_t ring = 0; ring <= rings; ring++) {
            float theta = (hemisphere ? 0.f : -Pi * .5f) + float(ring) / rings * Pi * .5f;
            for (uint32_t segment = 0; segment <= segments; segment++) {
                float phi = float(segment) / segments * 2 * Pi;
                vec3 n{cos(theta) * cos(phi), sin(theta), cos(theta) * sin(phi)}, p = n * .4f;
                p.y += hemisphere ? .6f : -.6f;
                vertices.push_back({vec4(p, 1), vec4(n, 0)});
            }
        }
    for (uint32_t r = 0; r < 2 * (rings + 1) - 1; r++)
        for (uint32_t s = 0; s < segments; s++) {
            uint32_t a = base + r * (segments + 1) + s, b = a + segments + 1;
            for (auto i : {a, b, a + 1, a + 1, b, b + 1})
                indices.push_back(i);
        }
    ranges[1].indexCount = uint32_t(indices.size()) - ranges[1].firstIndex;
    for (auto& v : vertices)
        v.previousPosition = v.position;
    return ranges;
}
} // namespace afterlight
