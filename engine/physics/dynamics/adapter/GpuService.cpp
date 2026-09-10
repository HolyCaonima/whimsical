#include "GpuService.h"
#include <algorithm>
#include <stdexcept>
namespace whimsical::dynamics {
GpuService::~GpuService() {
    mailbox_->close();
    for (auto& e : entries_)
        e.channel->retire();
}
void GpuService::run() {
    uint64_t revision = 0;
    while (mailbox_->wait(revision)) {
        drain();
        // All ready models have been submitted. Wait for their own fences, not
        // a display frame; no polling timer or renderer callback drives completion.
        for (auto& e : entries_)
            if (e.instance && e.instance->pending()) {
                e.instance->wait();
                Reply reply;
                reply.completed = e.instance->completed();
                reply.samples = e.instance->takeSamples();
                e.channel->complete(std::move(reply));
            }
    }
}
void GpuService::drain() {
    for (auto& channel : mailbox_->take())
        entries_.push_back({std::move(channel), {}});
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.channel->retired(); }),
        entries_.end());
    for (auto& e : entries_) {
        if (e.instance && e.instance->pending()) {
            if (e.instance->poll()) {
                Reply reply;
                reply.completed = e.instance->completed();
                reply.samples = e.instance->takeSamples();
                e.channel->complete(std::move(reply));
            }
            continue;
        }
        auto request = e.channel->take();
        if (!request)
            continue;
        try {
            Reply reply;
            switch (request->operation) {
            case Operation::Install:
                if (e.instance)
                    e.instance->install(request->plan);
                else
                    e.instance = std::make_unique<Instance>(core_, request->plan);
                break;
            case Operation::Apply:
                if (!e.instance)
                    throw std::logic_error("Compile the model before applying edits");
                e.instance->apply(request->commit);
                break;
            case Operation::Step:
                if (!e.instance)
                    throw std::logic_error("Compile the model before stepping");
                e.instance->step(request->tick);
                continue;
            case Operation::Read:
                if (!e.instance)
                    throw std::logic_error("Compile the model before readback");
                if (request->ranges.empty())
                    reply.values =
                        e.instance->read(request->field, request->set, request->first, request->count);
                else
                    reply.samples = e.instance->read(request->ranges);
                break;
            }
            reply.completed = e.instance->completed();
            e.channel->complete(std::move(reply));
        } catch (const std::exception& error) {
            Reply reply;
            reply.error = error.what();
            if (e.instance)
                reply.completed = e.instance->completed();
            e.channel->complete(std::move(reply));
        }
    }
}
} // namespace whimsical::dynamics
