#pragma once
#include "Types.h"
#include <mutex>
#include <condition_variable>
namespace whimsical {
// Latest-wins snapshot channel between the simulation and the renderer.
//
// Publishing never blocks on the GPU: a newer snapshot simply replaces an unread one,
// which bounds both latency and memory to a single frame. Acquiring does not clear the
// slot, so a renderer that outruns the simulation re-presents the newest snapshot
// instead of stalling for the next 60 Hz tick. Render rate is therefore limited by the
// display and the GPU, never by the simulation.
//
// Snapshots are immutable and handed over by reference count, so crossing the thread
// boundary costs no copy and the renderer can retain the previous frame for motion
// vectors without duplicating it.
enum class FrameStatus {
    Fresh,  // A newer snapshot was published; `frame` was replaced.
    Repeat, // Nothing new since the last acquire; `frame` still holds the newest one.
    Closed, // Producer is done; the renderer must stop.
};
class FrameMailbox {
    std::mutex mutex_;
    std::condition_variable ready_;
    FrameRef latest_;
    uint64_t sequence_ = 0, delivered_ = 0;
    bool closed_ = false;

  public:
    void publish(Frame frame) {
        // Allocate outside the lock; the simulation must not wait on the consumer. The
        // snapshot is held mutably only until it is stored, while it is still unshared.
        auto shared = std::make_shared<Frame>(std::move(frame));
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return;
        // Replacing an unread snapshot must not discard what it had to say. Folding its
        // events into this one keeps the delta chain unbroken across a drop, so the
        // renderer applies two frames' worth of changes instead of rebuilding the scene.
        // Merging and storing under one lock is what makes this correct: were the
        // consumer able to take the old snapshot in between, it would be handed a delta
        // reaching further back than its mirror and would rebuild anyway.
        if (latest_ && delivered_ != sequence_) {
            shared->delta.prepend(latest_->delta);
            shared->resetHistory = shared->resetHistory || latest_->resetHistory;
        }
        latest_ = std::move(shared);
        ++sequence_;
        ready_.notify_one();
    }
    // Blocks only until the first snapshot exists or the mailbox closes; never waits
    // for a *newer* one. `seen` is the caller's cursor and must persist across calls.
    FrameStatus acquire(FrameRef& frame, uint64_t& seen) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || latest_ != nullptr; });
        if (closed_)
            return FrameStatus::Closed;
        if (sequence_ == seen)
            return FrameStatus::Repeat;
        seen = sequence_;
        delivered_ = sequence_;
        frame = latest_;
        return FrameStatus::Fresh;
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }
};
} // namespace whimsical
