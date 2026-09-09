#pragma once
#include "SceneAsset.h"
#include "assets/AssetManager.h"
namespace whimsical {
class World;
class ScenePersistence {
    static void instantiate(World&, const SceneDocument&, AssetManager&);
    static void replace(World&, World& staged, const AssetRef&);

  public:
    struct CommittedError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    static uint32_t createEntity(World&, const SceneEntity&, AssetManager&, bool persistent = true);
    static void addComponents(World&, uint32_t, const SceneEntity&, AssetManager&);
    static SceneDocument capture(const World&, const AssetManager&);
    // Restore an authored document without starting scripts. source binds its /Game references.
    static void restore(World&, AssetManager&, const SceneDocument&, const AssetRef& source);
    // Prepare and validate resource edits before replacing the live resource table.
    static void setResources(World&, AssetManager&, const Json& patch);
    static Json resources(const World&, const AssetManager&);
    static void load(World&, AssetManager&, const AssetPath&);
    // Same path preserves the Map ID; Save As creates a new Map ID and keeps Object IDs.
    static AssetRef save(World&, AssetManager&, const AssetPath&, const std::string& name);
};
} // namespace whimsical
