#pragma once
#include "platform/Window.h"
#include "ui/Console.h"
#include <stdexcept>
#include <iostream>
namespace afterlight {
// Drives the real Win32 message -> text input -> console -> Frame route.
class ConsoleSmoke {
    unsigned stage_ = 0;
    static void check(bool value, const char* why) {
        if (!value)
            throw std::runtime_error(why);
    }
    static void key(HWND window, unsigned code) {
        PostMessageW(window, WM_KEYDOWN, code, 0);
        PostMessageW(window, WM_KEYUP, code, 0);
    }
    static void text(HWND window, const wchar_t* line) {
        while (*line)
            PostMessageW(window, WM_CHAR, *line++, 0);
    }

  public:
    void update(uint64_t frame, Window& window, ui::Console& console, ConsoleRegistry& vars) {
        auto hwnd = window.handle();
        if (stage_ == 0 && frame >= 12) {
            key(hwnd, VK_F10);
            ++stage_;
        } else if (stage_ == 1 && frame >= 22) {
            check(console.isOpen(), "Console smoke: F10 did not open console");
            text(hwnd, L"r.Hud 0\rr.  exp\t 0.75\rr.Exposure 999\rr. view\t 2\rfind r. "
                       L"view\rprofileGPU\rprofileGPU\rprofileCPU\rprofileCPU\r");
            ++stage_;
        } else if (stage_ == 2 && frame >= 42) {
            check(!vars.get<bool>("r.Hud") && vars.get<double>("r.Exposure") == .75 &&
                      vars.get<int>("r.DebugView") == 2,
                  "Console smoke: live CVars or invalid-value rejection failed");
            key(hwnd, VK_ESCAPE);
            ++stage_;
        } else if (stage_ == 3 && frame >= 62) {
            check(!console.isOpen(), "Console smoke: Escape did not close");
            key(hwnd, VK_F1);
            ++stage_;
        } else if (stage_ == 4 && frame >= 82) {
            check(vars.get<int>("r.DebugView") == 2, "Console smoke: removed F1 binding changed debug view");
            window.resizeClient(1100, 700);
            key(hwnd, VK_OEM_3);
            ++stage_;
        } else if (stage_ == 5 && frame >= 102) {
            check(console.isOpen(), "Console smoke: console cannot reopen with HUD disabled");
            text(hwnd, L"map.reload\rr.DebugView 0\rt.TimeScale 0\r");
            ++stage_;
        } else if (stage_ == 6 && frame >= 125) {
            check(vars.get<double>("t.TimeScale") == 0 && vars.get<int>("r.DebugView") == 0 &&
                      vars.get<double>("r.Exposure") == .75,
                  "Console smoke: final commands failed");
            text(hwnd, L"stat\rhelp r.Exposure\rprofileGPU\rprofileCPU\rr.");
            ++stage_;
        } else if (stage_ == 7 && frame >= 135) {
            key(hwnd, VK_DOWN);
            ++stage_;
        } else if (stage_ == 8 && frame >= 140) {
            check(console.view().input == vars.complete("r.").front() &&
                      console.view().selectedSuggestion == 0,
                  "Console smoke: Down did not select the first prediction");
            key(hwnd, VK_UP);
            ++stage_;
        } else if (stage_ == 9 && frame >= 145) {
            const auto view = console.view();
            check(view.input == vars.complete("r.").back() && view.selectedSuggestion >= 0 &&
                      view.suggestions.at(view.selectedSuggestion) == view.input,
                  "Console smoke: Up did not wrap and scroll the highlighted prediction");
            std::cout << "[ConsoleSmoke] Native arrow selection, fuzzy completion, input capture, invalid "
                         "values, HUD "
                         "independence and "
                         "unbound F1 passed\n";
            ++stage_;
        }
    }
    bool complete() const {
        return stage_ == 10;
    }
};
} // namespace afterlight
