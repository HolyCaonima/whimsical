#pragma once
#include "Registry.h"
#include "render/VulkanContext.h"

namespace afterlight::rg {
// Reflection describes operations, never coverage. Even a write-only shader may update
// just one pixel. A binding-only use (e.g. imageSize) needs storage but no old contents.
struct ShaderAccess {
    uint32_t binding = 0;
    Access access = Access::Compute;
    bool reads = false;
    bool writes = false;
};
struct Program {
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<ShaderAccess> accesses;
};
std::vector<ShaderAccess> reflect(const Registry&, const uint32_t* words, size_t count);
void merge(std::vector<ShaderAccess>& into, const std::vector<ShaderAccess>& from);
} // namespace afterlight::rg
