#pragma once
#include "AssetManager.h"
namespace whimsical {
// Composition root: the generic registry has no World/animation/backend dependencies.
void registerEngineAssets(AssetManager&);
} // namespace whimsical
