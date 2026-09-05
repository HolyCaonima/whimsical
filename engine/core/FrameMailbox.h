#pragma once
#include "Types.h"
#include <mutex>
#include <condition_variable>
#include <optional>
namespace afterlight {
// Single-slot latest-frame mailbox: immutable snapshots, bounded latency and memory.
// The simulation never waits for GPU work; the renderer owns all Vulkan objects.
class FrameMailbox {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Frame> latest_;
    bool closed_ = false;

  public:
    void publish(Frame frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return;
        latest_ = std::move(frame);
        ready_.notify_one();
    }
    bool consume(Frame& frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || latest_.has_value(); });
        if (!latest_)
            return false;
        frame = std::move(*latest_);
        latest_.reset();
        return true;
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }
};
} // namespace afterlight
