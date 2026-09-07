#include "RenderAuditWorker.h"
#include "core/CpuProfile.h"
#include <chrono>

namespace afterlight {
RenderAuditWorker::RenderAuditWorker(uint32_t width, uint32_t height) : width_(width), height_(height) {
    for (auto& buffer : buffers_)
        buffer.resize(size_t(width) * height * 4 * RenderAudit::signalCount);
    worker_ = std::thread([this] { run(); });
}
RenderAuditWorker::~RenderAuditWorker() {
    stop();
}
void RenderAuditWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    ready_.notify_all();
    if (worker_.joinable())
        worker_.join();
}
void RenderAuditWorker::submit(const uint16_t* values) {
    size_t slot;
    {
        CpuScope scope("Audit / Wait for CPU Slot");
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return !free_.empty() || error_; });
        if (error_)
            std::rethrow_exception(error_);
        slot = free_.front();
        free_.pop_front();
    }
    {
        CpuScope scope("Audit / Copy Completed Readback");
        auto& buffer = buffers_[slot];
        std::memcpy(buffer.data(), values, buffer.size() * sizeof(uint16_t));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(slot);
    }
    ready_.notify_all();
}
const RenderAudit& RenderAuditWorker::finish(const std::filesystem::path& output) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_ = output;
    }
    stop();
    if (error_)
        std::rethrow_exception(error_);
    return audit_;
}
void RenderAuditWorker::run() {
    try {
        for (;;) {
            size_t slot;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] { return closing_ || !pending_.empty(); });
                if (pending_.empty())
                    break;
                slot = pending_.front();
                pending_.pop_front();
            }
            const auto start = std::chrono::steady_clock::now();
            audit_.add(buffers_[slot].data(), size_t(width_) * height_);
            cpuMilliseconds_ += std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - start).count();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                free_.push_back(slot);
            }
            ready_.notify_all();
        }
        if (!output_.empty())
            audit_.save(output_, width_, height_);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::current_exception();
        }
        ready_.notify_all();
    }
}
} // namespace afterlight
