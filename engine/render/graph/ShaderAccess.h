#pragma once
#include "Registry.h"
#include "render/VulkanContext.h"

// What a compiled shader does to the resources it is bound to, read out of the SPIR-V that
// will actually run. The pipeline binds one descriptor set, so nothing in a shader's text
// says which resources a pass depends on; that used to be restated by hand next to every
// dispatch, where it could drift from the shader without anything noticing. Reflecting the
// module instead makes the declaration the shader itself, which is what the graph's
// dependencies, culling, storage reuse and barriers are derived from.
namespace afterlight::rg {
struct ShaderAccess {
    ResourceRef ref;
    Access access = Access::Compute;
    Usage usage = Usage::Read;
};

// A pipeline together with what its shaders touch: everything a pass needs to name in
// order to both record the work and declare it.
struct Program {
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<ShaderAccess> accesses;
};

// One module's worth. Reading a resource's contents is a Read, writing all of them an
// Overwrite, doing both a Modify; a resource whose declaration says one pass never
// replaces all of it is a Modify however few loads the shader does.
std::vector<ShaderAccess> reflect(const Registry&, const uint32_t* words, size_t count);
// A program is the union of its modules, and a pass that binds several programs is the
// union of those. Combining two accesses of one resource combines what they do to it.
void merge(std::vector<ShaderAccess>& into, const std::vector<ShaderAccess>& from);
} // namespace afterlight::rg
