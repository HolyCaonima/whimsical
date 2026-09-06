#include "StaticMesh.h"
#include <cstring>
#include <stdexcept>

namespace afterlight {
std::shared_ptr<StaticMesh> StaticMesh::decode(const std::string& bytes) {
    // STM1: counts followed by 15 float32 values per vertex and uint32 indices.
    if (bytes.size() < 12 || bytes.compare(0, 4, "STM1") != 0)
        throw std::invalid_argument("Invalid StaticMesh header");
    uint32_t counts[2];
    std::memcpy(counts, bytes.data() + 4, sizeof(counts));
    if (!counts[0] || !counts[1] || counts[1] % 3 ||
        bytes.size() != 12ull + uint64_t(counts[0]) * 60 + uint64_t(counts[1]) * 4)
        throw std::invalid_argument("Invalid StaticMesh payload size");
    auto mesh = std::make_shared<StaticMesh>();
    mesh->vertices.resize(counts[0]);
    const char* source = bytes.data() + 12;
    for (auto& v : mesh->vertices) {
        float f[15];
        std::memcpy(f, source, sizeof(f));
        source += sizeof(f);
        for (float value : f)
            if (!std::isfinite(value))
                throw std::invalid_argument("Nonfinite StaticMesh vertex");
        v = {{f[0], f[1], f[2]},
             {f[3], f[4], f[5]},
             {f[6], f[7]},
             {f[8], f[9], f[10], f[11]},
             {f[12], f[13], f[14]}};
    }
    mesh->indices.resize(counts[1]);
    std::memcpy(mesh->indices.data(), source, counts[1] * sizeof(uint32_t));
    for (auto index : mesh->indices)
        if (index >= counts[0])
            throw std::invalid_argument("StaticMesh index outside vertex buffer");
    return mesh;
}
std::shared_ptr<TextureAsset> TextureAsset::decode(const std::string& bytes, const Json& metadata) {
    if (bytes.size() < 12 || bytes.compare(0, 4, "TEX1") != 0)
        throw std::invalid_argument("Invalid Texture header");
    auto texture = std::make_shared<TextureAsset>();
    std::memcpy(&texture->width, bytes.data() + 4, 4);
    std::memcpy(&texture->height, bytes.data() + 8, 4);
    const uint64_t count = uint64_t(texture->width) * texture->height;
    if (!count || bytes.size() != 12 + count * 4)
        throw std::invalid_argument("Invalid Texture payload size");
    auto colorSpace = metadata.at("colorSpace").string();
    if (colorSpace != "sRGB" && colorSpace != "linear")
        throw std::invalid_argument("Texture colorSpace must be sRGB or linear");
    texture->srgb = colorSpace == "sRGB";
    texture->pixels.resize(size_t(count));
    std::memcpy(texture->pixels.data(), bytes.data() + 12, size_t(count) * 4);
    return texture;
}
} // namespace afterlight
