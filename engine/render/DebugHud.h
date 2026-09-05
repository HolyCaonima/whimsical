#pragma once
#include "core/Types.h"
#include "Renderer.h"
#include <windows.h>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>
namespace afterlight {
// A CPU-generated HUD texture; all GDI resources belong to the render thread.
// No HWND drawing or input ownership crosses the engine/render thread boundary.
class DebugHud {
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    uint8_t* pixels_ = nullptr;
    uint32_t w_ = 0, h_ = 0;
    void rectangle(int x, int y, int w, int h, COLORREF color) {
        RECT r{x, y, x + w, y + h};
        HBRUSH b = CreateSolidBrush(color);
        FillRect(dc_, &r, b);
        DeleteObject(b);
    }
    void text(int x, int y, int size, const std::string& value, COLORREF color = RGB(220, 230, 217),
              bool bold = false) {
        HFONT font = CreateFontA(-size, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                                 ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH, "Segoe UI");
        auto previous = SelectObject(dc_, font);
        SetTextColor(dc_, color);
        TextOutA(dc_, x, y, value.c_str(), int(value.size()));
        SelectObject(dc_, previous);
        DeleteObject(font);
    }

  public:
    ~DebugHud() {
        reset();
    }
    void reset() {
        if (dc_) {
            SelectObject(dc_, oldBitmap_);
            DeleteObject(bitmap_);
            DeleteDC(dc_);
        }
        dc_ = nullptr;
        bitmap_ = nullptr;
        pixels_ = nullptr;
    }
    void resize(uint32_t width, uint32_t height) {
        reset();
        w_ = width;
        h_ = height;
        dc_ = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = LONG(w_);
        info.bmiHeader.biHeight = -LONG(h_);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ =
            CreateDIBSection(dc_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr, 0);
        oldBitmap_ = SelectObject(dc_, bitmap_);
        SetBkMode(dc_, TRANSPARENT);
    }
    void draw(const Frame& f, void* output, const RenderStatistics& statistics, bool enabled) {
        std::memset(pixels_, 0, size_t(w_) * h_ * 4);
        if (enabled) {
            if (f.physicsDebug) {
                mat4 vp = f.camera.projection(float(w_) / h_) * f.camera.view();
                for (const auto& line : f.physicsLines) {
                    vec4 a = vp * vec4(line.a, 1), b = vp * vec4(line.b, 1);
                    if (a.w <= .1f || b.w <= .1f)
                        continue;
                    vec2 pa = (vec2(a) / a.w * .5f + .5f) * vec2(w_, h_),
                         pb = (vec2(b) / b.w * .5f + .5f) * vec2(w_, h_);
                    auto pen = CreatePen(
                        PS_SOLID, 1,
                        RGB(int(line.color.r * 255), int(line.color.g * 255), int(line.color.b * 255)));
                    auto old = SelectObject(dc_, pen);
                    MoveToEx(dc_, int(pa.x), int(pa.y), nullptr);
                    LineTo(dc_, int(pb.x), int(pb.y));
                    SelectObject(dc_, old);
                    DeleteObject(pen);
                }
            }
            int w = int(w_), h = int(h_);
            const COLORREF muted = RGB(134, 158, 147), gold = RGB(184, 171, 122), ink = RGB(15, 29, 29),
                           border = RGB(73, 93, 84), mint = RGB(117, 218, 187);
            rectangle(18, 17, 352, 70, ink);
            rectangle(18, 102, 244, 99, ink);
            rectangle(26, 28, 3, 49, gold);
            text(42, 23, 29, "A F T E R L I G H T", RGB(235, 233, 212));
            text(44, 58, 10, "A   W O R L D   W O R T H   R E P A I R I N G", muted);
            rectangle(28, 110, 42, 2, gold);
            text(28, 125, 11, "LOWER DISTRICT   /   04", muted);
            text(27, 144, 25, "The Rain Court", RGB(235, 240, 221), true);
            text(28, 179, 11, "AFTER THE RAIN     17:42", gold);
            if (w > 800) {
                int x = w - 238;
                rectangle(x, 24, 210, 205, ink);
                rectangle(x, 24, 210, 2, gold);
                text(x + 14, 40, 10, "LOCAL SIGNAL                  N", muted);
                for (const auto& e : f.mapObstacles) {
                    int px = x + 105 + int(e.position.x * 5.2f), py = 133 + int(e.position.z * 5.2f);
                    rectangle(px - int(e.size.x * 2.6f), py - int(e.size.z * 2.6f),
                              std::max(2, int(e.size.x * 5.2f)), std::max(2, int(e.size.z * 5.2f)), border);
                }
                int px = x + 105 + int(f.player.x * 5.2f), py = 133 + int(f.player.z * 5.2f);
                rectangle(px - 3, py - 3, 6, 6, mint);
                rectangle(x + 12, 200, 186, 1, border);
                text(x + 14, 210, 9, "THE RAIN COURT          04", muted);
                if (!f.path.empty()) {
                    auto pen = CreatePen(PS_SOLID, 1, gold);
                    auto old = SelectObject(dc_, pen);
                    MoveToEx(dc_, px, py, nullptr);
                    for (auto point : f.path)
                        LineTo(dc_, x + 105 + int(point.x * 5.2f), 133 + int(point.z * 5.2f));
                    SelectObject(dc_, old);
                    DeleteObject(pen);
                }
            }
            rectangle(28, h - 174, 320, 103, ink);
            rectangle(28, h - 174, 320, 1, border);
            rectangle(28, h - 174, 2, 103, gold);
            rectangle(44, h - 149, 42, 57, RGB(37, 68, 61));
            text(54, h - 138, 28, "K", mint, true);
            text(101, h - 158, 10, f.selected ? "ACTIVE COMPANION" : "COMPANION / UNSELECTED", muted);
            text(100, h - 142, 26, "Kiln", RGB(239, 240, 222), true);
            text(166, h - 130, 11, "/  " + f.locomotion, mint);
            text(101, h - 105, 11, "A warm core. A curious mind.", muted);
            text(30, h - 59, 12, f.message, RGB(218, 208, 162));
            if (f.hovered && f.hovered != f.selected && f.hovered <= f.entities.size())
                text(w / 2 - 100, h - 91, 14, "CLICK / E    " + f.entities[f.hovered - 1].name, gold, true);
            rectangle(0, h - 32, w, 32, RGB(12, 23, 24));
            text(28, h - 25, 11,
                 "WASD  Move     CLICK  Walk / interact     SHIFT  Run     CTRL  Crouch     E  Use",
                 RGB(193, 209, 196));
            if (w > 1000)
                text(w - 469, h - 25, 10, "MMB  Orbit   WHEEL  Zoom   F  Follow   F1  Views   ESC  Stop",
                     muted);
            static const char* modes[] = {"LIT",          "ALBEDO",          "NORMALS",
                                          "VIEW DEPTH",   "DIRECT RT (RAW)", "INDIRECT RT (RAW)",
                                          "RAW RADIANCE", "MOTION"};
            int statsY = w > 800 ? 235 : 24;
            rectangle(w - 238, statsY, 210, 99, ink);
            text(w - 224, statsY + 7, 10, modes[std::clamp(f.debugView, 0, 7)], muted);
            std::ostringstream fps, timing;
            if (statistics.fps >= 0) {
                fps << std::fixed << std::setprecision(1) << statistics.fps << " FPS";
                timing << std::fixed << std::setprecision(2) << "Frame " << statistics.frameMs << " ms   GPU "
                       << statistics.gpuMs << " ms";
            } else {
                fps << "-- FPS";
                timing << "Measuring frame time...";
            }
            text(w - 224, statsY + 24, 24, fps.str(), mint, true);
            text(w - 224, statsY + 56, 10, timing.str(), muted);
            text(w - 224, statsY + 77, 10, "60 FPS target  |  VSync ON", gold);
            if (f.physicsDebug) {
                rectangle(w - 238, statsY + 104, 210, 42, ink);
                text(w - 224, statsY + 109, 10, "F2  PHYSICS SCENE", mint);
                text(w - 224, statsY + 128, 9, "BLUE Ground  GOLD Solid  PINK Trigger", muted);
            }
        }
        GdiFlush();
        auto* dest = static_cast<uint8_t*>(output);
        for (size_t i = 0; i < size_t(w_) * h_; i++) {
            auto* src = pixels_ + i * 4;
            auto* dst = dest + i * 4;
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = (src[0] | src[1] | src[2]) ? 245 : 0;
        }
    }
};
} // namespace afterlight
