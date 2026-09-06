#pragma once
#include "core/Math.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace afterlight {
struct SkinVertex {
    vec3 position, normal, color;
    glm::uvec4 joints;
    vec4 weights;
};
struct SkinBinding {
    std::string joint;
    mat4 inverseBind;
};
// Mesh resources are independent of the animation solver; bindings resolve by skeleton joint name.
struct SkinnedMesh {
    std::vector<SkinVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SkinBinding> bindings;
    static std::shared_ptr<const SkinnedMesh> load(const std::filesystem::path&);
};
struct DeformedVertex {
    vec3 position, normal;
};
std::vector<DeformedVertex> deformSkin(const SkinnedMesh&, const std::vector<mat4>& palette);
} // namespace afterlight
