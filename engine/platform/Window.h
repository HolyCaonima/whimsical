#pragma once
#include "core/Types.h"
#include <windows.h>
namespace afterlight {
class Window {
    HWND hwnd_ = nullptr;
    Input input_;
    bool running_ = true;
    bool mouseKnown_ = false;
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);

  public:
    Window(uint32_t width, uint32_t height);
    ~Window();
    HWND handle() const {
        return hwnd_;
    }
    bool pump();
    Input input() const {
        return input_;
    }
    void consumeEdges();
    void title(const std::string&);
    void resizeClient(uint32_t, uint32_t);
    void minimize(bool);
};
} // namespace afterlight
