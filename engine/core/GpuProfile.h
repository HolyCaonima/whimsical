#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace whimsical {
// Value-only report crossing from the render thread to the console.
struct GpuScopeTiming {
    std::string name;
    int parent = -1;
    double milliseconds = 0;
    uint64_t samples = 1;
};
struct GpuProfile {
    uint64_t request = 0, frame = 0;
    uint32_t width = 0, height = 0;
    std::string device, error;
    std::vector<GpuScopeTiming> scopes;
    double windowMilliseconds = 0;
    uint64_t submissions = 0;
    std::string text() const;
    std::string json() const;
};
double gpuTimestampMilliseconds(uint64_t begin, uint64_t end, uint32_t validBits, double periodNanoseconds);
} // namespace whimsical
