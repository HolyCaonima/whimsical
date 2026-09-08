#include "GpuRenderTargets.h"
#include "graph/ImageReadback.h"
#include <cstring>

namespace afterlight {
namespace {
rg::Format graphFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::R32Uint:
        return rg::Format::R32Uint;
    case PixelFormat::R32Float:
        return rg::Format::R32F;
    case PixelFormat::RGBA8:
        return rg::Format::RGBA8;
    case PixelFormat::RGBA32Float:
        return rg::Format::RGBA32F;
    }
    throw std::invalid_argument("Unsupported RenderTarget pixel format");
}
VkFormat deviceFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::R32Uint:
        return VK_FORMAT_R32_UINT;
    case PixelFormat::R32Float:
        return VK_FORMAT_R32_SFLOAT;
    case PixelFormat::RGBA8:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::RGBA32Float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    throw std::invalid_argument("Unsupported RenderTarget pixel format");
}
} // namespace
GpuRenderTargets::GpuRenderTargets(VulkanContext& vk, rg::Registry& registry, rg::ResourcePool& pool)
    : vk_(vk), registry_(registry), pool_(pool), fixedResources_(registry.size()) {}
void GpuRenderTargets::destroy(Target& target) {
    vk_.destroy(target.color.image);
    vk_.destroy(target.depth.image);
    target.color.state = {};
    target.depth.state = {};
}
GpuRenderTargets::~GpuRenderTargets() {
    // Renderer has waited for the device; all completed pixels are delivered before teardown.
    collect();
    failed();
    for (auto& entry : targets_) {
        entry.second.owner->invalidate();
        destroy(entry.second);
    }
}
void GpuRenderTargets::failed() {
    submitted_ = false;
    for (auto& r : reads_) {
        r.request->cancel("render-failed");
        vk_.destroy(r.staging);
    }
    reads_.clear();
}
void GpuRenderTargets::prepare(const std::vector<std::shared_ptr<RenderTargetResource>>& outputs,
                               const std::vector<std::shared_ptr<PixelReadRequest>>& requests, uint32_t width,
                               uint32_t height, uint64_t frame, uint64_t tick) {
    registry_.truncateImports(fixedResources_);
    outputs_.clear();
    for (auto it = targets_.begin(); it != targets_.end();) {
        if (!it->second.owner->valid()) {
            destroy(it->second);
            it = targets_.erase(it);
        } else
            ++it;
    }
    for (const auto& output : outputs) {
        if (!output->valid())
            continue;
        auto& target = targets_[output.get()];
        target.owner = output;
        const auto& asset = *output->asset;
        const auto w = asset.width ? asset.width : width, h = asset.height ? asset.height : height;
        if (w > vk_.properties.limits.maxImageDimension2D || h > vk_.properties.limits.maxImageDimension2D)
            throw std::runtime_error("RenderTarget dimensions exceed device limit");
        if (target.color.image.width != w || target.color.image.height != h) {
            destroy(target);
            target.color.image =
                vk_.image(w, h, deviceFormat(asset.format),
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_STORAGE_BIT);
            target.depth.image =
                vk_.image(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            target.contents = {};
            target.contents.rtVersion = target.color.image.generation;
            target.contents.width = w;
            target.contents.height = h;
        }
        ++target.contents.contentVersion;
        target.contents.renderFrame = frame;
        target.contents.sourceTick = tick;
        target.written = true;
    }
    // Import retained contents too: a read is independent of whether a producer runs this frame.
    for (auto& entry : targets_) {
        auto& t = entry.second;
        const auto& owner = t.owner;
        rg::Declaration d;
        d.name = "RenderTarget/" + owner->handle;
        d.lifetime = rg::Lifetime::Imported;
        d.format = graphFormat(owner->asset->format);
        t.colorId = registry_.declare(d);
        d.name += "/Depth";
        d.format = rg::Format::D32;
        t.depthId = registry_.declare(d);
        if (t.written)
            outputs_.push_back({t.colorId, t.depthId});
    }
    uint64_t readBytes = 0;
    for (const auto& request : requests) {
        if (!request->claim())
            continue;
        auto reject = [&](const char* status, TextureContents contents = {}) {
            request->complete({status, contents, request->region, {}});
        };
        if (!request->target->valid()) {
            reject("invalidated");
            continue;
        }
        auto found = targets_.find(request->target.get());
        if (found == targets_.end()) {
            reject("uninitialized");
            continue;
        }
        const auto& t = found->second;
        const auto& contents = t.contents;
        if (!request->expectedVersion.empty() &&
            request->expectedVersion != std::to_string(contents.rtVersion)) {
            reject("stale-version", contents);
            continue;
        }
        PixelRegion region = request->region;
        if (region.x >= contents.width || region.y >= contents.height) {
            reject("out-of-bounds", contents);
            continue;
        }
        if (!region.width)
            region.width = contents.width - region.x;
        if (!region.height)
            region.height = contents.height - region.y;
        if (region.width > contents.width - region.x || region.height > contents.height - region.y) {
            reject("out-of-bounds", contents);
            continue;
        }
        const uint64_t bytes =
            uint64_t(region.width) * region.height * pixelBytes(request->target->asset->format);
        if (readBytes + bytes > 64 * 1024 * 1024) {
            reject("readback-budget-exceeded", contents);
            continue;
        }
        readBytes += bytes;
        reads_.emplace_back();
        auto& r = reads_.back();
        r.request = request;
        r.result = {"ready", contents, region, {}};
        r.source = t.colorId;
        rg::Declaration d;
        d.name = "ReadPixels/" + request->handle;
        d.kind = rg::Kind::Buffer;
        d.lifetime = rg::Lifetime::Imported;
        d.handover = rg::Access::Host;
        r.destination = registry_.declare(d);
        r.staging = vk_.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
    }
    pool_.syncImports();
    for (auto& entry : targets_) {
        auto& t = entry.second;
        pool_.importImage(t.colorId, t.color.image, t.color.state);
        pool_.importImage(t.depthId, t.depth.image, t.depth.state);
    }
    for (auto& r : reads_)
        pool_.importBuffer(r.destination, r.staging);
}
void GpuRenderTargets::addReadbacks(rg::RenderGraph& graph) {
    for (const auto& r : reads_) {
        const auto& region = r.result.region;
        rg::addImageReadback(graph, "Texture Pixel Readback", r.destination,
                             {{r.source, 0, region.x, region.y, region.width, region.height}});
    }
}
void GpuRenderTargets::collect() {
    if (!submitted_)
        return;
    submitted_ = false;
    for (auto& entry : targets_) {
        auto& t = entry.second;
        if (t.written) {
            t.owner->publish(t.contents);
            t.written = false;
        }
    }
    for (auto& r : reads_) {
        r.result.bytes.resize(size_t(r.staging.size));
        std::memcpy(r.result.bytes.data(), r.staging.mapped, r.result.bytes.size());
        r.request->complete(std::move(r.result));
        vk_.destroy(r.staging);
    }
    reads_.clear();
}
} // namespace afterlight
