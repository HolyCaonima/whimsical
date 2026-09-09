#pragma once
#include "Registry.h"
#include <memory>

namespace whimsical::rg {
// Reflection describes operations, never coverage. Even a write-only shader may update
// just one pixel. A binding-only use (e.g. imageSize) needs storage but no old contents.
struct ShaderAccess {
    uint32_t binding = 0;
    Access access = Access::Compute;
    bool reads = false;
    bool writes = false;
};
struct ProgramStorage;
struct Program {
    std::shared_ptr<const ProgramStorage> storage;
    std::vector<ShaderAccess> accesses;
    Extent3D localSize;
};
Extent3D reflectLocalSize(const uint32_t* words, size_t count);
std::vector<ShaderAccess> reflect(const Registry&, const uint32_t* words, size_t count);
void merge(std::vector<ShaderAccess>& into, const std::vector<ShaderAccess>& from);
} // namespace whimsical::rg
