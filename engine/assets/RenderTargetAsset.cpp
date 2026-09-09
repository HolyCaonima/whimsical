#include "RenderTargetAsset.h"
#include <atomic>
#include <stdexcept>

namespace whimsical {
RenderTargetAsset::RenderTargetAsset()
    : generation([] {
          static std::atomic<uint64_t> next{0};
          return ++next;
      }()) {}
const char* pixelFormatName(PixelFormat format) {
    switch (format) {
    case PixelFormat::R32Uint:
        return "R32Uint";
    case PixelFormat::R32Float:
        return "R32Float";
    case PixelFormat::RGBA8:
        return "RGBA8";
    case PixelFormat::RGBA32Float:
        return "RGBA32Float";
    }
    throw std::invalid_argument("Unknown pixel format");
}
uint32_t pixelBytes(PixelFormat format) {
    return format == PixelFormat::RGBA32Float ? 16 : 4;
}
std::shared_ptr<RenderTargetAsset> RenderTargetAsset::decode(const std::string& bytes) {
    auto j = Json::parse(bytes);
    auto a = std::make_shared<RenderTargetAsset>();
    a->width = j.at("width").uint();
    a->height = j.at("height").uint();
    if ((!a->width) != (!a->height))
        throw std::invalid_argument("RenderTarget dimensions must both be positive or both zero");
    const auto& name = j.at("format").string();
    for (auto format :
         {PixelFormat::R32Uint, PixelFormat::R32Float, PixelFormat::RGBA8, PixelFormat::RGBA32Float})
        if (name == pixelFormatName(format)) {
            a->format = format;
            return a;
        }
    throw std::invalid_argument("Unsupported RenderTarget format: " + name);
}
} // namespace whimsical
