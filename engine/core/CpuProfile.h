#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
namespace whimsical {
struct CpuScopeTiming {
    std::string name;
    int parent = -1;
    double milliseconds = 0, selfMilliseconds = 0;
};
struct CpuThreadProfile {
    std::string thread;
    std::vector<CpuScopeTiming> scopes;
};
// Immutable snapshot data after capture. Thread trees have independent clocks/roots;
// they must not be added together to derive frame latency.
struct CpuProfile {
    uint64_t request = 0, frame = 0, tick = 0;
    std::vector<CpuThreadProfile> threads;
    std::string text() const;
    std::string json() const;
};
class CpuScope;
class CpuProfiler {
    friend class CpuScope;
    using Clock = std::chrono::steady_clock;
    static thread_local CpuProfiler* current_;
    CpuProfiler* previous_ = nullptr;
    bool active_ = false;
    CpuThreadProfile result_;
    std::vector<int> stack_;
    Clock::time_point start_;
    int begin(const char* name);
    void end(int index, Clock::time_point start);

  public:
    CpuProfiler(bool enabled, const char* thread, const char* root);
    ~CpuProfiler();
    CpuProfiler(const CpuProfiler&) = delete;
    CpuProfiler& operator=(const CpuProfiler&) = delete;
    CpuThreadProfile finish();
};
// The thread-local capture binding lets engine modules add scopes without depending
// on the console or renderer. Disabled scopes allocate nothing and read no clock.
class CpuScope {
    CpuProfiler* profiler_;
    int index_ = -1;
    std::chrono::steady_clock::time_point start_;

  public:
    explicit CpuScope(const char* name);
    ~CpuScope();
    void finish();
    CpuScope(const CpuScope&) = delete;
    CpuScope& operator=(const CpuScope&) = delete;
};
} // namespace whimsical
