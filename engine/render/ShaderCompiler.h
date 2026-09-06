#pragma once
#include "assets/ShaderAsset.h"
#include <filesystem>
#include <map>
#include <array>

namespace afterlight {
// Ray queries execute in compute shaders, so link the live Shader set into each
// compute pass. Raster links a single Shader. There are no Material permutations.
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
        return programs_.size();
    }

  private:
    std::map<std::string, std::vector<uint32_t>> programs_;
    std::filesystem::path directory_;
};
} // namespace afterlight
