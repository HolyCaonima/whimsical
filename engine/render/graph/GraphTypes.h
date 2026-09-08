#pragma once
#include "Registry.h"
#include "render/VulkanContext.h"

// Vulkan vocabulary of the render graph: what a pass does with a resource, and the
// stage/access/layout triple the compiler derives from it. Features name intent; only this
// file turns intent into synchronisation.
namespace afterlight::rg {
enum class Access : uint32_t {
    None = 0,
    ComputeRead = 1u << 0,
    ComputeWrite = 1u << 1,
    GraphicsRead = 1u << 2, // descriptor read from a vertex or fragment shader
    ColorWrite = 1u << 3,
    DepthWrite = 1u << 4,
    TransferRead = 1u << 5,
    TransferWrite = 1u << 6,
    VertexBuffer = 1u << 7,
    IndexBuffer = 1u << 8,
    BuildRead = 1u << 9,  // acceleration structure build input
    BuildWrite = 1u << 10, // acceleration structure build output
    TraceRead = 1u << 11, // acceleration structure read by a ray query
    Present = 1u << 12,
    ComputeReadWrite = ComputeRead | ComputeWrite,
};
inline Access operator|(Access a, Access b) {
    return Access(uint32_t(a) | uint32_t(b));
}
inline Access& operator|=(Access& a, Access b) {
    a = a | b;
    return a;
}
inline bool has(Access set, Access bit) {
    return (uint32_t(set) & uint32_t(bit)) != 0;
}
inline bool writes(Access a) {
    return has(a, Access::ComputeWrite | Access::ColorWrite | Access::DepthWrite |
                      Access::TransferWrite | Access::BuildWrite);
}

struct AccessInfo {
    VkPipelineStageFlags2 stage = 0;
    VkAccessFlags2 access = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
AccessInfo accessInfo(Access);

// Per physical resource, carried across frames so the first access of frame N is correctly
// ordered against the last access of frame N-1. Ping-ponged history and aliased transients
// depend on that being true. Image layout stays in Image::layout, its only home.
struct AccessState {
    VkPipelineStageFlags2 writeStage = 0;
    VkAccessFlags2 writeAccess = 0;
    VkPipelineStageFlags2 readStages = 0;    // reads since the last write, for write-after-read
    VkPipelineStageFlags2 flushedStages = 0; // reads already made visible to the last write
    VkAccessFlags2 flushedAccess = 0;
};
} // namespace afterlight::rg
