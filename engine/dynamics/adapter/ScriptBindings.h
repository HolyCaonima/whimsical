#pragma once
#include "Mailbox.h"
#include <duktape.h>
#include <map>
namespace whimsical::dynamics {
class ScriptBindings {
    struct Entry {
        Model model;
        std::shared_ptr<Channel> channel;
        uint64_t version = 0;
        bool needsPlan = true;
        SolverPolicy policy;
        Operation pendingOperation = Operation::Install;
    };
    std::shared_ptr<Mailbox> mailbox_;
    std::vector<SpaceRef> spaces_;
    std::vector<RelationRef> types_;
    std::map<uint32_t, std::unique_ptr<Entry>> models_;
    uint32_t next_ = 0;
    duk_ret_t dispatch(duk_context*, int);
    static duk_ret_t call(duk_context*);

  public:
    ScriptBindings(duk_context*, std::shared_ptr<Mailbox>);
    ~ScriptBindings();
};
} // namespace whimsical::dynamics
