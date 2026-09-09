#pragma once
#include "assets/ShaderAsset.h"
#include "renderCore/ShaderCompiler.h"
#include <filesystem>
#include <map>
#include <array>

namespace whimsical {
// Ray queries execute in compute shaders, so link the live Shader set into each
// compute pass. Raster links a single Shader. Material render state is runtime data
// or fixed-function pipeline state, never generated surface-code permutations.
class ShaderCompiler {
  public:
    using ShaderSet = std::vector<std::shared_ptr<const ShaderAsset>>;
    inline static constexpr std::array<const char*, 6> surfacePasses = {
        "lighting.comp", "reuse.comp", "di_temporal.comp", "di_spatial.comp", "resolve.comp", "di_gradient.comp"};
    static std::string surfaceLibrary(const ShaderSet&, bool raster);
    static std::string passSource(const std::filesystem::path& directory, const std::string& pass,
                                  const ShaderSet&);
    const std::vector<uint32_t>& compile(const std::string& pass, const ShaderSet&);
    size_t compilationCount() const {
        return compiler_.compilationCount();
    }

  private:
    rc::ShaderCompiler compiler_;
};
} // namespace whimsical
