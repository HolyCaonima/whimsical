#include "SkinnedMesh.h"
#include <fstream>
#include <stdexcept>
namespace afterlight {
std::shared_ptr<const SkinnedMesh> SkinnedMesh::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    auto read = [&](void* p, size_t n) {
        if (!file.read(static_cast<char*>(p), std::streamsize(n)))
            throw std::runtime_error("Cannot read skin mesh: " + path.string());
    };
    auto u = [&]() {
        uint32_t v;
        read(&v, 4);
        return v;
    };
    char magic[4];
    read(magic, 4);
    if (std::string(magic, 4) != "SKN1")
        throw std::runtime_error("Invalid skin mesh format");
    auto nv = u(), ni = u(), nb = u();
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->bindings.resize(nb);
    for (auto& b : mesh->bindings) {
        b.joint.resize(u());
        read(b.joint.data(), b.joint.size());
        read(glm::value_ptr(b.inverseBind), 64);
    }
    mesh->vertices.resize(nv);
    mesh->indices.resize(ni);
    for (auto& v : mesh->vertices) {
        read(&v.position, 12);
        read(&v.normal, 12);
        read(&v.color, 12);
        read(&v.joints, 16);
        read(&v.weights, 16);
        for (int k = 0; k < 4; ++k)
            if (v.joints[k] >= nb)
                throw std::runtime_error("Skin references missing binding");
    }
    read(mesh->indices.data(), ni * 4);
    for (auto index : mesh->indices)
        if (index >= nv)
            throw std::runtime_error("Skin index out of bounds");
    return mesh;
}
std::vector<DeformedVertex> deformSkin(const SkinnedMesh& mesh, const std::vector<mat4>& palette) {
    if (palette.size() != mesh.bindings.size())
        throw std::invalid_argument("Skin palette mismatch");
    std::vector<glm::mat3> normals;
    normals.reserve(palette.size());
    for (const auto& p : palette)
        normals.push_back(glm::transpose(glm::inverse(glm::mat3(p))));
    std::vector<DeformedVertex> result;
    result.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) {
        vec3 p(0), n(0);
        for (int k = 0; k < 4; ++k) {
            p += v.weights[k] * vec3(palette[v.joints[k]] * vec4(v.position, 1));
            n += v.weights[k] * (normals[v.joints[k]] * v.normal);
        }
        result.push_back({p, glm::normalize(n)});
    }
    return result;
}
} // namespace afterlight
