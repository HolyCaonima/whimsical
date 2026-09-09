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
    out << "ProfileGPU | request " << request;
    if (windowMilliseconds > 0)
        out << " | RenderCore window " << windowMilliseconds << " ms | " << submissions << " submissions";
    else
        out << " | frame " << frame << " | " << width << "x" << height;
    out << " | " << device << '\n';
    if (!error.empty())
        out << "Error: " << error << '\n';
    out << (windowMilliseconds > 0
                ? "GPU timestamp intervals (sum ms / % of sum / count / mean ms); children are included in parents.\n"
                : "GPU timestamp intervals (inclusive ms / % of frame); child times are included in parents.\n");
    const double total = scopes.empty() ? 0 : scopes.front().milliseconds;
    std::vector<int> depths;
    for (const auto& scope : scopes) {
        int depth = scope.parent < 0 ? 0 : depths.at(scope.parent) + 1;
        depths.push_back(depth);
        out << std::fixed << std::setprecision(3) << std::setw(9) << scope.milliseconds << " ms  "
            << std::setprecision(1) << std::setw(5) << (total > 0 ? scope.milliseconds / total * 100 : 0)
            << "%  ";
        if (windowMilliseconds > 0)
            out << std::setw(6) << scope.samples << " x " << std::setprecision(3) << std::setw(8)
                << (scope.samples ? scope.milliseconds / scope.samples : 0) << " ms  ";
        out << std::string(size_t(depth) * 2, ' ') << scope.name << '\n';
    }
    if (windowMilliseconds > 0)
        out << "All GraphContext submissions recorded in the sampling window; per-context counts may differ.\n"
               "Sum of GPU intervals, not wall-clock duration or device utilization. Excludes CPU waits, "
               "Present and native submissions outside execution graphs.\n";
    else
        out << "One graph submission. Excludes CPU work, Present and other graph submissions.\n";
    return out.str();
}
std::string GpuProfile::json() const {
    Json::Array rows;
    for (const auto& scope : scopes)
        rows.push_back({{"name", scope.name}, {"parent", scope.parent}, {"inclusiveMs", scope.milliseconds}, {"samples", double(scope.samples)}});
    return Json{{"request", double(request)},
                {"frame", double(frame)},
                {"windowMs", windowMilliseconds},
                {"submissions", double(submissions)},
                {"width", width},
                {"height", height},
                {"device", device},
                {"error", error},
                {"timing", "GPU timestamp intervals, inclusive milliseconds"},
                {"scopes", Json::array(std::move(rows))}}
        .dump();
}
} // namespace whimsical
