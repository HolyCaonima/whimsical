#pragma once
#include "renderCore/vulkan/VulkanAccess.h"
#include "core/RenderTarget.h"
#include "renderCore/vulkan/GraphAccess.h"

namespace whimsical {
// Render-thread owner of runtime texture storage and asynchronous readback staging.
// It knows pixel formats and producers' outputs, never Entity IDs or JavaScript values.
class GpuRenderTargets {
  public:
    struct RasterOutput {
        rg::ResourceId color, depth;
    };
    GpuRenderTargets(VulkanContext&, rg::Registry&, rc::NativeResources);
    ~GpuRenderTargets();
    void prepare(const std::vector<std::shared_ptr<RenderTargetResource>>& outputs,
                 const std::vector<std::shared_ptr<PixelReadRequest>>& requests, uint32_t width,
                 uint32_t height, uint64_t frame, uint64_t tick);
    const std::vector<RasterOutput>& rasterOutputs() const {
        return outputs_;
    }
    void addReadbacks(rg::RenderGraph&);
    void collect(); // Only after the existing frame fence; before any resource reuse/destruction.
    void submitted() {
        submitted_ = true;
    }
    void failed();

  private:
    struct TrackedImage {
        Image image;
        rg::AccessState state;
    };
    struct Target {
        // Retain CPU identity until the render thread retires invalidated GPU storage.
        std::shared_ptr<RenderTargetResource> owner;
        TrackedImage color, depth;
        TextureContents contents;
        rg::ResourceId colorId, depthId;
        bool written = false;
    };
    struct Readback {
        std::shared_ptr<PixelReadRequest> request;
        PixelReadResult result;
        Buffer staging;
        rg::ResourceId source, destination;
    };
    VulkanContext& vk_;
    rg::Registry& registry_;
    rc::NativeResources pool_;
    const size_t fixedResources_;
    std::map<RenderTargetResource*, Target> targets_;
    std::vector<RasterOutput> outputs_;
    std::vector<Readback> reads_;
    bool submitted_ = false;
    void destroy(Target&);
};
} // namespace whimsical
