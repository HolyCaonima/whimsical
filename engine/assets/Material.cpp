#include "Material.h"
#include "AssetManager.h"
#include "StaticMesh.h"
#include <algorithm>

namespace whimsical {
namespace {
const std::vector<const char*> domains{"surface", "display"};
const std::vector<const char*> surfaces{"opaque", "masked"};
const std::vector<const char*> culls{"none", "back", "front"};
const std::vector<const char*> comparisons{"never",   "less",     "equal",        "lessEqual",
                                           "greater", "notEqual", "greaterEqual", "always"};
const std::vector<const char*> blends{"opaque", "alpha", "additive"};
template <class T>
void readEnum(const Json& data, const char* key, const std::vector<const char*>& names, T& value) {
    if (!data.contains(key))
        return;
    const auto& name = data.at(key).string();
    auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end())
        throw std::invalid_argument(std::string("Invalid Material ") + key + ": " + name);
    value = T(found - names.begin());
}
} // namespace
Json MaterialRenderState::json() const {
    return {{"domain", domains[int(domain)]}, {"surface", surfaces[int(surface)]},
            {"cull", culls[int(cull)]},       {"depthTest", depthTest},
            {"depthWrite", depthWrite},       {"depthCompare", comparisons[int(depthCompare)]},
            {"blend", blends[int(blend)]},    {"alphaCutoff", alphaCutoff},
            {"rayVisible", rayVisible},       {"layer", layer}};
}
MaterialRenderState MaterialRenderState::fromJson(const Json& data) {
    MaterialRenderState result;
    readEnum(data, "domain", domains, result.domain);
    readEnum(data, "surface", surfaces, result.surface);
    readEnum(data, "cull", culls, result.cull);
    readEnum(data, "depthCompare", comparisons, result.depthCompare);
    readEnum(data, "blend", blends, result.blend);
    if (data.contains("depthTest"))
        result.depthTest = data.at("depthTest").boolean();
    if (data.contains("depthWrite"))
        result.depthWrite = data.at("depthWrite").boolean();
    if (data.contains("alphaCutoff"))
        result.alphaCutoff = float(data.at("alphaCutoff").number());
    if (data.contains("rayVisible"))
        result.rayVisible = data.at("rayVisible").boolean();
    if (data.contains("layer"))
        result.layer = data.at("layer").uint();
    if (result.alphaCutoff < 0 || result.alphaCutoff > 1)
        throw std::invalid_argument("Material alpha cutoff must be in [0, 1]");
    if (result.domain == MaterialDomain::Surface && (result.layer || result.blend != MaterialBlend::Opaque))
        throw std::invalid_argument("Deferred surfaces require layer 0 and opaque blending");
    return result;
}
Json MaterialDefinition::json() const {
    Json result{{"shader", shader.json()}, {"properties", properties}, {"textures", Json::object()}};
    for (const auto& texture : textures)
        result["textures"][texture.first] = texture.second.json();
    result["renderState"] = renderState.json();
    return result;
}
MaterialDefinition MaterialDefinition::fromJson(const Json& data) {
    MaterialDefinition result;
    if (data.contains("renderState"))
        result.renderState = MaterialRenderState::fromJson(data.at("renderState"));
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
    result.renderState = renderState;
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
    result.renderState = material.renderState;
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
