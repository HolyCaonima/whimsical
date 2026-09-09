#pragma once
#include <duktape.h>
#include <memory>
#include <functional>
#include <string>
namespace whimsical::ui {
class UiCore;
}
namespace whimsical {
// Owns only this JS realm's documents and callbacks. Engine UI survives map reloads.
class UiBindings {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    UiBindings(duk_context*, ui::UiCore&, std::function<void(const std::string&)> log,
               std::function<std::string(const std::string&)> resolve, std::function<int(int)> invoke);
    ~UiBindings();
    void setVisible(bool);
};
} // namespace whimsical
