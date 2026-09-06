#include "EngineAssets.h"
#include "animation/ai4animation/Controller.h"
#include "animation/SkinnedMesh.h"
#include "scene/SceneAsset.h"
#include "MaterialAsset.h"
#include <sstream>
namespace afterlight {
void registerEngineAssets(AssetManager& manager) {
    manager.registerLoader("StaticMesh", [](AssetManager&, const AssetHeader&, const std::string& bytes) {
        return StaticMesh::decode(bytes);
    });
    manager.registerLoader("Texture", [](AssetManager&, const AssetHeader& h, const std::string& bytes) {
        return TextureAsset::decode(bytes, h.metadata);
    });
    manager.registerLoader("Shader", [](AssetManager&, const AssetHeader& h, const std::string& bytes) {
        return ShaderAsset::decode(h.metadata, bytes);
    });
    manager.registerLoader("Material", [](AssetManager& manager, const AssetHeader&, const std::string& bytes) {
        auto asset = std::make_shared<MaterialAsset>();
        asset->parameters = MaterialDefinition::fromJson(Json::parse(bytes)).resolve(manager);
        return asset;
    });
    namespace ai = animation::ai4animation;
    manager.registerLoader("OnnxModel", [](AssetManager&, const AssetHeader&, const std::string& bytes) {
        return std::make_shared<ai::OnnxModel>(bytes);
    });
    manager.registerLoader("SkinnedMesh", [](AssetManager&, const AssetHeader&, const std::string& bytes) {
        std::istringstream stream(bytes, std::ios::binary);
        return SkinnedMesh::decode(stream);
    });
    manager.registerLoader("AnimationController", [](AssetManager& manager, const AssetHeader& header,
                                                     const std::string& bytes) {
        auto network = manager.load<ai::OnnxModel>(AssetRef::fromJson(header.metadata.at("network")));
        auto post = manager.load<ai::OnnxModel>(AssetRef::fromJson(header.metadata.at("postprocessor")));
        std::istringstream stream(bytes, std::ios::binary);
        return std::make_shared<ai::ControllerResource>(ai::ControllerAsset::decode(stream, network, post));
    });
    manager.registerLoader("Map", [](AssetManager&, const AssetHeader&, const std::string& bytes) {
        auto asset = std::make_shared<SceneAsset>();
        asset->scene = SceneDocument::fromJson(Json::parse(bytes));
        return asset;
    });
}
} // namespace afterlight
