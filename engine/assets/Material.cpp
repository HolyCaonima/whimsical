#include "Material.h"
#include "AssetManager.h"
#include "StaticMesh.h"
#include <algorithm>

namespace whimsical {
Json MaterialDefinition::json() const {
    Json result{{"shader", shader.json()}, {"properties", properties}, {"textures", Json::object()}};
    for (const auto& texture : textures)
        result["textures"][texture.first] = texture.second.json();
    return result;
}
MaterialDefinition MaterialDefinition::fromJson(const Json& data) {
    MaterialDefinition result;
    result.shader = AssetRef::fromJson(data.at("shader"));
    result.properties = data.at("properties");
    // Material values use GPU float precision; canonicalize at the document
    // boundary so load/capture/save has the same representation as inline authoring.
    for (const auto& property : data.at("properties").members()) {
        if (std::holds_alternative<double>(property.second.value()))
            result.properties[property.first] = float(property.second.number());
        else {
            Json value = Json::array();
            for (const auto& component : property.second.elements())
                value.push(float(component.number()));
            result.properties[property.first] = value;
        }
    }
    for (const auto& texture : data.at("textures").members())
        result.textures.emplace(texture.first, AssetRef::fromJson(texture.second));
    return result;
}
Material MaterialDefinition::resolve(AssetManager& assets) const {
    Material result;
    result.shader = assets.load<ShaderAsset>(shader);
    const auto& schema = *result.shader;
    for (const auto& property : properties.members())
        if (std::none_of(schema.properties.begin(), schema.properties.end(),
                         [&](const ShaderProperty& p) { return p.name == property.first; }))
            throw std::invalid_argument("Unknown Shader property: " + property.first);
    for (const auto& property : schema.properties) {
        vec4 value = property.value;
        if (properties.contains(property.name)) {
            const auto& data = properties.at(property.name);
            int count = int(property.type);
            if (count == 1)
                value.x = float(data.number());
            else {
                if (data.elements().size() != size_t(count))
                    throw std::invalid_argument("Material property type mismatch: " + property.name);
                for (int i = 0; i < count; ++i)
                    value[i] = float(data.at(i).number());
            }
        }
        result.properties.push_back(value);
    }
    for (const auto& texture : textures)
        if (std::find(schema.textures.begin(), schema.textures.end(), texture.first) == schema.textures.end())
            throw std::invalid_argument("Unknown Shader texture: " + texture.first);
    for (const auto& name : schema.textures) {
        auto found = textures.find(name);
        result.textures.push_back(found == textures.end() ? nullptr
                                                          : assets.load<TextureAsset>(found->second));
    }
    return result;
}
MaterialDefinition MaterialDefinition::capture(const Material& material, const AssetManager& assets) {
    if (!material.shader)
        throw std::invalid_argument("Cannot save a Material without a Shader");
    MaterialDefinition result;
    result.shader = assets.resolve(material.shader->reference());
    for (size_t i = 0; i < material.properties.size(); ++i) {
        const auto& schema = material.shader->properties[i];
        Json value = Json::array();
        if (schema.type == PropertyType::Float)
            value = material.properties[i].x;
        else
            for (int j = 0; j < int(schema.type); ++j)
                value.push(material.properties[i][j]);
        result.properties[schema.name] = value;
    }
    for (size_t i = 0; i < material.textures.size(); ++i)
        if (material.textures[i])
            result.textures.emplace(material.shader->textures[i],
                                    assets.resolve(material.textures[i]->reference()));
    return result;
}
} // namespace whimsical
