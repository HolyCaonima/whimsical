#include "Mailbox.h"
#include <stdexcept>
namespace whimsical::dynamics {
void WorkSignal::notify() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++revision;
    }
    condition.notify_one();
}
void Channel::submit(Request r) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_)
        throw std::runtime_error("Dynamics channel is closed");
    if (busy_)
        throw std::runtime_error(
            "Dynamics instance is busy; consume its reply before submitting another operation");
    request_ = std::move(r);
    busy_ = true;
    signal_->notify();
}
std::optional<Reply> Channel::receive() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto out = std::move(reply_);
    reply_.reset();
    if (out)
        busy_ = false;
    return out;
}
std::optional<Request> Channel::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto out = std::move(request_);
    request_.reset();
    return out;
}
void Channel::complete(Reply r) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reply_ = std::move(r);
    }
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(signal_->mutex);
        wake = signal_->completion;
    }
    if (wake)
        wake();
}
void Channel::retire() {
    std::lock_guard<std::mutex> lock(mutex_);
    retired_ = true;
    request_.reset();
    signal_->notify();
}
bool Channel::retired() {
    std::lock_guard<std::mutex> lock(mutex_);
    return retired_;
}
bool Channel::busy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}
std::shared_ptr<Channel> Mailbox::attach() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
        throw std::runtime_error("Dynamics GPU service has stopped");
    auto channel = std::make_shared<Channel>(signal_);
    new_.push_back(channel);
    signal_->notify();
    return channel;
}
std::vector<std::shared_ptr<Channel>> Mailbox::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto out = std::move(new_);
    new_.clear();
    return out;
}
void Mailbox::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    for (auto& channel : new_)
        channel->retire();
    new_.clear();
    {
        std::lock_guard<std::mutex> signalLock(signal_->mutex);
        signal_->stopped = true;
    }
    signal_->condition.notify_all();
}
bool Mailbox::wait(uint64_t& revision) {
    std::unique_lock<std::mutex> lock(signal_->mutex);
    signal_->condition.wait(lock, [&] { return signal_->stopped || signal_->revision != revision; });
    revision = signal_->revision;
    return !signal_->stopped;
}
void Mailbox::wakeOnCompletion(std::function<void()> wake) {
    std::lock_guard<std::mutex> lock(signal_->mutex);
    signal_->completion = std::move(wake);
}
} // namespace whimsical::dynamics
