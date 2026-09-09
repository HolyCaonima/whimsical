#include "GpuProfile.h"
#include "assets/Json.h"
#include <iomanip>
#include <sstream>
#include <stdexcept>
namespace whimsical {
double gpuTimestampMilliseconds(uint64_t begin, uint64_t end, uint32_t validBits, double period) {
    if (!validBits || validBits > 64)
        throw std::invalid_argument("Unsupported GPU timestamp width");
    const uint64_t mask = validBits == 64 ? UINT64_MAX : (uint64_t(1) << validBits) - 1;
    return double((end - begin) & mask) * period / 1e6;
}
std::string GpuProfile::text() const {
    std::ostringstream out;
    out << "ProfileGPU | request " << request << " | frame " << frame << " | " << width << "x" << height
        << " | " << device << '\n';
    if (!error.empty())
        out << "Error: " << error << '\n';
    out << "GPU timestamp intervals (inclusive ms / % of frame); child times are included in parents.\n";
    const double total = scopes.empty() ? 0 : scopes.front().milliseconds;
    std::vector<int> depths;
    for (const auto& scope : scopes) {
        int depth = scope.parent < 0 ? 0 : depths.at(scope.parent) + 1;
        depths.push_back(depth);
        out << std::fixed << std::setprecision(3) << std::setw(9) << scope.milliseconds << " ms  "
            << std::setprecision(1) << std::setw(5) << (total > 0 ? scope.milliseconds / total * 100 : 0)
            << "%  " << std::string(size_t(depth) * 2, ' ') << scope.name << '\n';
    }
    out << "Single graphics/compute queue. Excludes CPU work, Present duration and startup resource "
           "submissions.\n";
    return out.str();
}
std::string GpuProfile::json() const {
    Json::Array rows;
    for (const auto& scope : scopes)
        rows.push_back({{"name", scope.name}, {"parent", scope.parent}, {"inclusiveMs", scope.milliseconds}});
    return Json{{"request", double(request)},
                {"frame", double(frame)},
                {"width", width},
                {"height", height},
                {"device", device},
                {"error", error},
                {"timing", "GPU timestamp intervals, inclusive milliseconds"},
                {"scopes", Json::array(std::move(rows))}}
        .dump();
}
} // namespace whimsical
