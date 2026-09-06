#include "Window.h"
#include <windowsx.h>
#include <mmsystem.h>
#include <stdexcept>
namespace afterlight {
Window::Window(uint32_t width, uint32_t height) {
    SetProcessDPIAware();
    // Windows defaults to a 15.6 ms timer tick, which quantizes every sub-frame wait the
    // simulation performs. Without this the publish cadence depends on whether some other
    // process happens to have raised the global timer resolution.
    timeBeginPeriod(1);
    input_.width = width;
    input_.height = height;
    WNDCLASSW wc{};
    wc.lpfnWndProc = procedure;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AfterlightWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"AFTERLIGHT | The Rain Court", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr,
                            nullptr, wc.hInstance, this);
    if (!hwnd_)
        throw std::runtime_error("Window creation failed");
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}
Window::~Window() {
    if (hwnd_)
        DestroyWindow(hwnd_);
    timeEndPeriod(1);
}
LRESULT CALLBACK Window::procedure(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        w = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
    }
    if (!w)
        return DefWindowProcW(h, msg, wp, lp);
    auto& i = w->input_;
    switch (msg) {
    case WM_CLOSE:
        w->running_ = false;
        return 0;
    case WM_SIZE:
        i.width = LOWORD(lp);
        i.height = HIWORD(lp);
        return 0;
    case WM_SETFOCUS:
        i.focused = true;
        return 0;
    case WM_KILLFOCUS:
        i.focused = false;
        i.keys.fill(false);
        i.pressed.fill(false);
        i.text.clear();
        w->highSurrogate_ = 0;
        i.left = i.right = i.middle = false;
        w->mouseKnown_ = false;
        ReleaseCapture();
        return 0;
    case WM_CHAR:
        if (wp == 22) { // Ctrl+V, copied as text rather than executed by the platform layer.
            if (OpenClipboard(h)) {
                auto data = GetClipboardData(CF_UNICODETEXT);
                if (data) {
                    auto* text = static_cast<const wchar_t*>(GlobalLock(data));
                    if (text) {
                        for (size_t n = 0; text[n] && n < 4096; ++n)
                            w->appendCharacter(text[n] < 32 ? L' ' : text[n]);
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
            }
        } else
            w->appendCharacter(wchar_t(wp));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp < 256) {
            if (!i.keys[wp])
                i.pressed[wp] = true;
            i.keys[wp] = true;
        }
        if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
            w->running_ = false;
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wp < 256)
            i.keys[wp] = false;
        return 0;
    case WM_MOUSEMOVE: {
        float x = float(GET_X_LPARAM(lp)), y = float(GET_Y_LPARAM(lp));
        if (w->mouseKnown_) {
            i.deltaX += x - i.mouseX;
            i.deltaY += y - i.mouseY;
        }
        i.mouseX = x;
        i.mouseY = y;
        w->mouseKnown_ = true;
        return 0;
    }
    case WM_MOUSEWHEEL:
        i.wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        return 0;
    case WM_LBUTTONDOWN:
        i.left = true;
        i.leftPressed = true;
        SetFocus(h);
        return 0;
    case WM_LBUTTONUP:
        i.left = false;
        return 0;
    case WM_RBUTTONDOWN:
        i.right = true;
        i.rightPressed = true;
        return 0;
    case WM_RBUTTONUP:
        i.right = false;
        return 0;
    case WM_MBUTTONDOWN:
        i.middle = true;
        SetCapture(h);
        return 0;
    case WM_MBUTTONUP:
        i.middle = false;
        ReleaseCapture();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
bool Window::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return running_;
}
void Window::waitForMessages(uint32_t milliseconds) const {
    if (!milliseconds)
        return;
    // MWMO_INPUTAVAILABLE also returns for messages already queued but not yet removed,
    // so a message that arrived between pump() and this call cannot be missed.
    MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}
void Window::wake() const {
    if (hwnd_)
        PostMessageW(hwnd_, WM_NULL, 0, 0);
}
void Window::consumeEdges() {
    input_.text.clear();
    input_.pressed.fill(false);
    input_.leftPressed = input_.rightPressed = false;
    input_.deltaX = input_.deltaY = input_.wheel = 0;
}
void Window::appendCharacter(wchar_t c) {
    if (c >= 0xD800 && c <= 0xDBFF) {
        highSurrogate_ = c;
        return;
    }
    char32_t value = c;
    if (c >= 0xDC00 && c <= 0xDFFF) {
        if (!highSurrogate_)
            return;
        value = 0x10000 + ((highSurrogate_ - 0xD800) << 10) + c - 0xDC00;
    }
    highSurrogate_ = 0;
    input_.text.push_back(value);
}
void Window::releaseGameInput() {
    input_.keys.fill(false);
    input_.pressed.fill(false);
    input_.left = input_.right = input_.middle = input_.leftPressed = input_.rightPressed = false;
    input_.deltaX = input_.deltaY = input_.wheel = 0;
    ReleaseCapture();
}
void Window::title(const std::string& t) {
    SetWindowTextA(hwnd_, t.c_str());
}
void Window::resizeClient(uint32_t width, uint32_t height) {
    RECT r{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void Window::minimize(bool value) {
    ShowWindow(hwnd_, value ? SW_MINIMIZE : SW_RESTORE);
}
} // namespace afterlight
