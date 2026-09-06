#pragma once
#include "core/Input.h"
#include <windows.h>
namespace afterlight {
class Window {
    HWND hwnd_ = nullptr;
    Input input_;
    bool running_ = true;
    bool mouseKnown_ = false;
    wchar_t highSurrogate_ = 0;
    void appendCharacter(wchar_t);
    void uiEvent(ui::InputEvent::Type, uint32_t code = 0, float x = 0, float y = 0);
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);

  public:
    Window(uint32_t width, uint32_t height);
    ~Window();
    HWND handle() const {
        return hwnd_;
    }
    bool pump();
    // Sleeps until a message arrives or the timeout expires, whichever comes first.
    // Frame pacing depends on this actually honouring millisecond timeouts, which is why
    // the window raises the process timer resolution for its lifetime.
    void waitForMessages(uint32_t milliseconds) const;
    // Wakes a thread blocked in waitForMessages. Safe to call from any thread.
    void wake() const;
    Input input() const {
        return input_;
    }
    void consumeEdges();
    void releaseGameInput();
    void title(const std::string&);
    void resizeClient(uint32_t, uint32_t);
    void minimize(bool);
};
} // namespace afterlight
