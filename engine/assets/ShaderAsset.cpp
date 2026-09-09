#include "ShaderAsset.h"
#include <set>
#include <regex>
#include <stdexcept>

namespace whimsical {
std::shared_ptr<ShaderAsset> ShaderAsset::decode(const Json& metadata, const std::string& source) {
    auto shader = std::make_shared<ShaderAsset>();
    shader->source = source;
    if (source.empty())
        throw std::invalid_argument("Shader source is empty");
    if (metadata.at("materialModel").string() != "metallicRoughness")
        throw std::invalid_argument("Unsupported Shader material model");
    const std::map<std::string, PropertyType> types = {{"float", PropertyType::Float},
                                                       {"vec2", PropertyType::Vec2},
                                                       {"vec3", PropertyType::Vec3},
                                                       {"vec4", PropertyType::Vec4}};
    std::set<std::string> names;
    auto name = [&](const std::string& value) {
        if (!std::regex_match(value, std::regex("[A-Za-z][A-Za-z0-9_]*")) ||
            value.find("__") != std::string::npos || value.compare(0, 3, "gl_") == 0 ||
            !names.insert(value).second)
            throw std::invalid_argument("Invalid or duplicate Shader schema name: " + value);
    };
    for (const auto& p : metadata.at("properties").elements()) {
        ShaderProperty property{p.at("name").string(), types.at(p.at("type").string())};
        name(property.name);
        auto count = size_t(property.type);
        const auto& value = p.at("default");
        if (count == 1)
            property.value.x = float(value.number());
        else {
            if (value.elements().size() != count)
                throw std::invalid_argument("Shader default does not match property type");
            for (size_t i = 0; i < count; ++i)
                property.value[int(i)] = float(value.at(i).number());
        }
        shader->properties.push_back(property);
    }
    names.clear(); // Properties and textures occupy separate GLSL structs.
    for (const auto& t : metadata.at("textures").elements()) {
        name(t.string());
        shader->textures.push_back(t.string());
    }
    if (shader->properties.size() > MaxProperties || shader->textures.size() > MaxTextures)
        throw std::invalid_argument(
            "Shader schema exceeds the surface ABI capacity (8 properties / 8 textures)");
    if (metadata.contains("renderState"))
        throw std::invalid_argument("Shader renderState moved to Material assets");
    return shader;
}
} // namespace whimsical
