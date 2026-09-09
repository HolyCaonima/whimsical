#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace whimsical::rc {
// Compiles a complete GPU program. Material linking and application include generation
// belong to the caller; this compiler has no knowledge of scenes or rendering passes.
class ShaderCompiler {
  public:
    const std::vector<uint32_t>& compile(const std::string& name, const std::string& source);
    size_t compilationCount() const { return programs_.size(); }

  private:
    std::map<std::string, std::vector<uint32_t>> programs_;
    std::filesystem::path directory_;
};
} // namespace whimsical::rc
