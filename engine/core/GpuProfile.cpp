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
    if (!group.empty())
        out << " | " << group << " | " << submissions << " submissions";
    else
        out << " | frame " << frame << " | " << width << "x" << height;
    out << " | " << device << '\n';
    if (!error.empty())
        out << "Error: " << error << '\n';
    if (scopes.empty())
        out << "No GPU submissions captured for this selection.\n";
    out << "GPU timestamp intervals (inclusive ms / % of system); children are included in parents.\n";
    double total = 0;
    std::vector<int> depths;
    for (const auto& scope : scopes) {
        int depth = scope.parent < 0 ? 0 : depths.at(scope.parent) + 1;
        depths.push_back(depth);
        if (scope.parent < 0) {
            total = scope.milliseconds;
            if (!group.empty()) {
                out << "\n[" << scope.name << "] " << std::fixed << std::setprecision(3) << total << " ms | "
                    << scope.samples << " submission(s)\n";
                continue;
            }
        }
        out << std::fixed << std::setprecision(3) << std::setw(9) << scope.milliseconds << " ms  "
            << std::setprecision(1) << std::setw(5) << (total > 0 ? scope.milliseconds / total * 100 : 0)
            << "%  " << std::string(size_t(depth) * 2, ' ') << scope.name;
        if (scope.samples > 1)
            out << " (" << scope.samples << " calls, " << std::setprecision(3)
                << scope.milliseconds / scope.samples << " ms mean)";
        out << '\n';
    }
    out << "One submission per selected context; percentages are local to each system.\n"
           "Systems are sampled independently. Excludes CPU work and Present; totals are not device "
           "utilization.\n";
    return out.str();
}
std::string GpuProfile::json() const {
    Json::Array rows;
    for (const auto& scope : scopes)
        rows.push_back({{"name", scope.name},
                        {"parent", scope.parent},
                        {"inclusiveMs", scope.milliseconds},
                        {"samples", double(scope.samples)}});
    return Json{{"request", double(request)},
                {"frame", double(frame)},
                {"group", group},
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
