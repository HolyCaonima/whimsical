#include "CpuProfile.h"
#include "assets/Json.h"
#include <cassert>
#include <iomanip>
#include <sstream>
namespace afterlight {
thread_local CpuProfiler* CpuProfiler::current_ = nullptr;
CpuProfiler::CpuProfiler(bool enabled, const char* thread, const char* root) : active_(enabled) {
    if (active_) {
        previous_ = current_;
        current_ = this;
        result_.thread = thread;
        start_ = Clock::now();
        begin(root);
    }
}
CpuProfiler::~CpuProfiler() {
    if (active_)
        current_ = previous_;
}
int CpuProfiler::begin(const char* name) {
    int index = int(result_.scopes.size());
    result_.scopes.push_back({name, stack_.empty() ? -1 : stack_.back()});
    stack_.push_back(index);
    return index;
}
void CpuProfiler::end(int index, Clock::time_point start) {
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    assert(!stack_.empty() && stack_.back() == index);
    stack_.pop_back();
    auto& scope = result_.scopes[index];
    scope.milliseconds = ms;
    scope.selfMilliseconds += ms;
    if (scope.parent >= 0)
        result_.scopes[scope.parent].selfMilliseconds -= ms;
}
CpuThreadProfile CpuProfiler::finish() {
    if (active_) {
        assert(stack_.size() == 1);
        end(0, start_);
        current_ = previous_;
        active_ = false;
    }
    return std::move(result_);
}
CpuScope::CpuScope(const char* name) : profiler_(CpuProfiler::current_) {
    if (profiler_) {
        start_ = std::chrono::steady_clock::now();
        index_ = profiler_->begin(name);
    }
}
CpuScope::~CpuScope() {
    finish();
}
void CpuScope::finish() {
    if (profiler_)
        profiler_->end(index_, start_);
    profiler_ = nullptr;
}
std::string CpuProfile::text() const {
    std::ostringstream out;
    out << "ProfileCPU | request " << request << " | render frame " << frame << " | game tick " << tick
        << '\n';
    out << "CPU wall-clock intervals: inclusive / self ms. Wait scopes include blocked time.\n";
    for (const auto& thread : threads) {
        out << "[" << thread.thread << "]\n";
        std::vector<int> depths;
        for (const auto& scope : thread.scopes) {
            const int depth = scope.parent < 0 ? 0 : depths.at(scope.parent) + 1;
            depths.push_back(depth);
            out << std::fixed << std::setprecision(3) << std::setw(9) << scope.milliseconds << " ms / "
                << std::setw(9) << scope.selfMilliseconds << " ms  " << std::string(size_t(depth) * 2, ' ')
                << scope.name << '\n';
        }
    }
    out << "Game and Render can overlap; do not sum thread totals. Self excludes instrumented children; "
           "this is elapsed time, not OS CPU utilization or GPU execution time.\n";
    return out.str();
}
std::string CpuProfile::json() const {
    Json::Array lanes;
    for (const auto& thread : threads) {
        Json::Array rows;
        for (const auto& scope : thread.scopes)
            rows.push_back({{"name", scope.name},
                            {"parent", scope.parent},
                            {"inclusiveMs", scope.milliseconds},
                            {"selfMs", scope.selfMilliseconds}});
        lanes.push_back({{"thread", thread.thread}, {"scopes", Json::array(std::move(rows))}});
    }
    return Json{{"request", double(request)},
                {"frame", double(frame)},
                {"tick", double(tick)},
                {"timing", "CPU wall-clock intervals, inclusive and self milliseconds; includes waits"},
                {"threads", Json::array(std::move(lanes))}}
        .dump();
}
} // namespace afterlight
