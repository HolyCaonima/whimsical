#pragma once
#include "Mailbox.h"
#include "dynamics/runtime/Instance.h"
namespace whimsical::dynamics {
// Created/drained/destroyed on the application's existing GPU owner thread.
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
};
} // namespace whimsical::dynamics
