#pragma once
#include "physics/dynamics/compiler/CompiledPlan.h"
#include "physics/dynamics/runtime/State.h"
#include <mutex>
#include <condition_variable>
#include <functional>
namespace whimsical::dynamics {
enum class Operation { Install, Apply, Step, Read };
struct Request {
    Operation operation;
    PlanRef plan;
    ModelCommit commit;
    TickInput tick;
    StateField field = StateField::Value;
    SetId set = 0;
    uint32_t first = 0, count = 0;
    std::vector<StateRange> ranges;
};
struct Reply {
    CompletedState completed;
    std::vector<float> values;
    std::string error;
    std::vector<std::vector<float>> samples;
};
// Reliable single-flight channel per model. A second operation returns Busy until
// the caller consumes the first reply; replacing a render Frame never drops a tick.
struct WorkSignal {
    std::mutex mutex;
    std::condition_variable condition;
    uint64_t revision = 0;
    bool stopped = false;
    std::function<void()> completion;
    void notify();
};
class Channel {
    std::shared_ptr<WorkSignal> signal_;
    std::mutex mutex_;
    std::optional<Request> request_;
    std::optional<Reply> reply_;
    bool busy_ = false, retired_ = false;

  public:
    explicit Channel(std::shared_ptr<WorkSignal> signal) : signal_(std::move(signal)) {}
    void submit(Request);
    std::optional<Reply> receive();
    std::optional<Request> take();
    void complete(Reply);
    void retire();
    bool retired();
    bool busy();
};
class Mailbox {
    std::shared_ptr<WorkSignal> signal_ = std::make_shared<WorkSignal>();
    std::mutex mutex_;
    std::vector<std::shared_ptr<Channel>> new_;
    bool closed_ = false;

  public:
    std::shared_ptr<Channel> attach();
    std::vector<std::shared_ptr<Channel>> take();
    void close();
    bool wait(uint64_t& revision);
    void wakeOnCompletion(std::function<void()>);
};
} // namespace whimsical::dynamics
