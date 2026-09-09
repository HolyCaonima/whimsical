#pragma once
#include "renderCore/vulkan/GraphAccess.h"

namespace whimsical::rg {
struct ImageReadback {
    ResourceRef source;
    VkDeviceSize offset = VK_WHOLE_SIZE; // Append after the preceding image when omitted.
    uint32_t x = 0, y = 0, width = 0, height = 0;
};
// Uses the same transfer -> Host handover as screenshots and render audits. Buffer memory
// is coherent Readback memory; the caller copies it only after the submission fence signals.
inline void addImageReadback(RenderGraph& graph, const char* name, ResourceId destination,
                             std::vector<ImageReadback> copies) {
    auto pass = graph.add(name);
    for (const auto& copy : copies)
        pass.read(copy.source, Access::Transfer);
    pass.overwrite(destination, Access::Transfer)
        .record([destination, copies = std::move(copies)](const PassContext& c) {
            VkDeviceSize nextOffset = 0;
            for (const auto& copy : copies) {
                auto& image = c.image(copy.source);
                VkBufferImageCopy region{};
                region.bufferOffset = copy.offset == VK_WHOLE_SIZE ? nextOffset : copy.offset;
                const auto format = c.pool->declaration(copy.source.id).format;
                region.imageSubresource = {
                    VkImageAspectFlags(format == Format::D32 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                    0, 0, 1};
                region.imageOffset = {int32_t(copy.x), int32_t(copy.y), 0};
                region.imageExtent = {copy.width ? copy.width : image.width,
                                      copy.height ? copy.height : image.height, 1};
                nextOffset =
                    region.bufferOffset + VkDeviceSize(region.imageExtent.width) * region.imageExtent.height *
                                              formatInfo(format).bytes;
                if (nextOffset > c.buffer(destination).size)
                    throw std::invalid_argument("Image readback exceeds the destination buffer");
                vkCmdCopyImageToBuffer(c.command, image.handle, image.layout, c.buffer(destination).handle, 1,
                                       &region);
            }
        });
}
} // namespace whimsical::rg
