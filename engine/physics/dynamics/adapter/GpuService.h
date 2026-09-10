#pragma once
#include "Mailbox.h"
#include "physics/dynamics/runtime/Instance.h"
namespace whimsical::dynamics {
// One owner thread per service, independent of rendering/presentation.
class GpuService {
    struct Entry {
        std::shared_ptr<Channel> channel;
        std::unique_ptr<Instance> instance;
    };
    rc::RenderCore& core_;
    std::shared_ptr<Mailbox> mailbox_;
    std::vector<Entry> entries_;

  public:
    GpuService(rc::RenderCore& core, std::shared_ptr<Mailbox> mailbox)
        : core_(core), mailbox_(std::move(mailbox)) {}
    ~GpuService();
    void drain();
    void run(); // Event-driven owner loop; returns when the mailbox closes.
};
} // namespace whimsical::dynamics
