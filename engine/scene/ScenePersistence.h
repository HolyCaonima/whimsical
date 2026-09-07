#pragma once
#include "SceneAsset.h"
#include "assets/AssetManager.h"
namespace afterlight {
class World;
class ScenePersistence {
    static void instantiate(World&, const SceneDocument&, AssetManager&);

  public:
    static uint32_t createEntity(World&, const SceneEntity&, AssetManager&);
    static void addComponents(World&, uint32_t, const SceneEntity&, AssetManager&);
    static SceneDocument capture(const World&, const AssetManager&);
    static void load(World&, AssetManager&, const AssetPath&);
    // Same path preserves the Map ID; Save As creates a new Map ID and keeps Object IDs.
    static AssetRef save(World&, AssetManager&, const AssetPath&, const std::string& name);
};
} // namespace afterlight
