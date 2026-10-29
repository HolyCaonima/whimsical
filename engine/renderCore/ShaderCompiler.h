#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <mutex>

namespace whimsical::rc {
// Compiles a complete GPU program. Material linking and application include generation
// belong to the caller; this compiler has no knowledge of scenes or rendering passes.
class ShaderCompiler {
  public:
    const std::vector<uint32_t>& compile(const std::string& name, const std::string& source);
    // (name, source) pairs; uncached programs compile concurrently. Results keep their order.
    std::vector<const std::vector<uint32_t>*> compile(
        const std::vector<std::pair<std::string, std::string>>& sources);
    size_t compilationCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return programs_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<uint32_t>> programs_;
};
} // namespace whimsical::rc
