#pragma once
#include "RenderAudit.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace whimsical {
// Lossless, bounded CPU consumer. The renderer hands over completed GPU readbacks;
// only this worker touches the accumulator and output files. Two reusable CPU slots
// overlap copying with analysis without growing memory or dropping regression samples.
class RenderAuditWorker {
  public:
    RenderAuditWorker(uint32_t width, uint32_t height);
    ~RenderAuditWorker();
    void submit(const uint16_t* values);
    const RenderAudit& finish(const std::filesystem::path& output = {});
    double cpuMilliseconds() const { return cpuMilliseconds_; } // Read after finish.

  private:
    void run();
    void stop();
    uint32_t width_, height_;
    std::array<std::vector<uint16_t>, 2> buffers_;
    std::deque<size_t> free_{0, 1}, pending_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool closing_ = false;
    std::exception_ptr error_;
    std::filesystem::path output_;
    RenderAudit audit_;
    double cpuMilliseconds_ = 0;
    std::thread worker_;
};
} // namespace whimsical
