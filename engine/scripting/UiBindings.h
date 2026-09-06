#pragma once
#include <duktape.h>
#include <memory>
#include <functional>
#include <string>
namespace afterlight::ui {
class UiCore;
}
namespace afterlight {
// Owns only this JS realm's documents and callbacks. Engine UI survives map reloads.
class UiBindings {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    UiBindings(duk_context*, ui::UiCore&, std::function<void(const std::string&)> log);
    ~UiBindings();
    void setVisible(bool);
};
} // namespace afterlight
