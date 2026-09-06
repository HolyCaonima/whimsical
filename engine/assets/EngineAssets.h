#pragma once
#include "AssetManager.h"
namespace afterlight {
// Composition root: the generic registry has no World/animation/backend dependencies.
void registerEngineAssets(AssetManager&);
} // namespace afterlight
