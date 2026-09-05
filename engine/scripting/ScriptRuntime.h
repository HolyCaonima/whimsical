#pragma once
#include "core/World.h"
#include <duktape.h>
#include <thread>
namespace afterlight {
class ScriptRuntime {
    duk_context* context_ = nullptr;
    World& world_;
    std::thread::id owner_;
    void evaluateFile(const std::string&);
    void checkedCall(int args);

  public:
    explicit ScriptRuntime(World&);
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    void initialize();
    void execute(const std::string& source, const std::string& label = "runtime");
    void tick(float dt, const Input& input);
};
} // namespace afterlight
