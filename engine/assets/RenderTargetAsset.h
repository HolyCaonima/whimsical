#pragma once
#include "Asset.h"

namespace whimsical {
enum class PixelFormat { R32Uint, R32Float, RGBA8, RGBA32Float };
const char* pixelFormatName(PixelFormat);
uint32_t pixelBytes(PixelFormat);
// An immutable allocation description. Neither pixels nor GPU/JS handles belong to an asset.
struct RenderTargetAsset final : Asset {
    RenderTargetAsset();
    const uint64_t generation;
    uint32_t width = 0, height = 0; // Both zero: follow the current view's pixel dimensions.
    PixelFormat format = PixelFormat::R32Uint;
    static std::shared_ptr<RenderTargetAsset> decode(const std::string&);
};
} // namespace whimsical
