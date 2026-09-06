#pragma once
#include "core/Types.h"
#include "Renderer.h"
#include "ui/AnimationInspector.h"
#include <windows.h>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>
namespace afterlight {
// A CPU-generated HUD texture; all GDI resources belong to the render thread.
// No HWND drawing or input ownership crosses the engine/render thread boundary.
//
// The overlay is a full-resolution CPU raster target, so redrawing it every frame would
// make the engine's cost scale with resolution rather than with the scene. It is instead
// keyed on a signature of everything it actually displays: `draw` reports whether the
// upload buffer was rewritten, and the renderer skips the GPU upload when it was not.
// Steady-state repaint rate is ~2 Hz, driven by the statistics interval.
class DebugHud {
    // FNV-style mixer over exactly the values the overlay renders.
    struct Signature {
        uint64_t value = 1469598103934665603ull;
        void mix(uint64_t v) {
            value ^= v + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
        }
        void text(const std::string& s) {
            mix(s.size());
            for (char c : s)
                mix(uint64_t(uint8_t(c)));
        }
        void real(double v) {
            uint64_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            mix(bits);
        }
    };
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    uint8_t* pixels_ = nullptr;
    uint32_t w_ = 0, h_ = 0;
    uint64_t signature_ = 0;
    bool signatureValid_ = false;
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
        auto wide = unicode(value);
        TextOutW(dc_, x, y, wide.c_str(), int(wide.size()));
        SelectObject(dc_, previous);
        DeleteObject(font);
    }
    static std::wstring unicode(const std::string& value) {
        int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
        std::wstring result(size_t(n), L' ');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), result.data(), n);
        return result;
    }
    void console(const ConsoleView& view) {
        if (!view.open)
            return;
        const int bottom = std::max(220, int(h_) * 3 / 5);
        rectangle(0, 0, int(w_), bottom, RGB(14, 20, 28));
        rectangle(0, bottom - 2, int(w_), 2, RGB(90, 210, 177));
        text(18, 12, 17, "AFTERLIGHT  /  CONSOLE", RGB(114, 227, 195), true);
        text(18, 36, 12,
             "~ / F10  Toggle    Esc  Close    Up / Down  Select / History    Enter  Execute    Tab  "
             "Complete");
        const int suggestions = int(view.suggestions.size()) * 20;
        const int rows = std::max(0, (bottom - 100 - suggestions) / 18);
        int start = std::max(0, int(view.lines.size()) - rows), y = 59;
        SaveDC(dc_);
        IntersectClipRect(dc_, 14, 55, int(w_) - 14, bottom - 40 - suggestions);
        for (int n = start; n < int(view.lines.size()); ++n, y += 18)
            text(18, y, 14, view.lines[n].text,
                 view.lines[n].error ? RGB(255, 148, 130) : RGB(202, 215, 227));
        RestoreDC(dc_, -1);
        if (suggestions) {
            SaveDC(dc_);
            IntersectClipRect(dc_, 14, 55, int(w_) - 14, bottom - 40);
            for (int i = 0; i < int(view.suggestions.size()); ++i) {
                const int top = bottom - 40 - suggestions + i * 20;
                const bool selected = i == view.selectedSuggestion;
                if (selected)
                    rectangle(14, top, int(w_) - 28, 20, RGB(42, 77, 76));
                text(20, top + 1, 14, std::string(selected ? "> " : "  ") + view.suggestions[i],
                     selected ? RGB(226, 255, 242) : RGB(114, 227, 195), selected);
            }
            RestoreDC(dc_, -1);
        }
        if (view.scroll)
            text(int(w_) - 160, 14, 12, "SCROLL +" + std::to_string(view.scroll));
        rectangle(14, bottom - 35, int(w_) - 28, 27, RGB(27, 37, 48));
        auto prefix = unicode("> " + view.beforeCursor);
        auto font =
            CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
        auto previous = SelectObject(dc_, font);
        SIZE extent{};
        GetTextExtentPoint32W(dc_, prefix.c_str(), int(prefix.size()), &extent);
        SelectObject(dc_, previous);
        DeleteObject(font);
        int shift = std::max(0, int(extent.cx) - int(w_) + 60);
        SaveDC(dc_);
        IntersectClipRect(dc_, 18, bottom - 35, int(w_) - 18, bottom - 8);
        text(20 - shift, bottom - 32, 16, "> " + view.input, RGB(238, 244, 250));
        rectangle(20 + int(extent.cx) - shift, bottom - 30, 1, 17, RGB(114, 227, 195));
        RestoreDC(dc_, -1);
    }
    static std::string presentLabel(const RenderStatistics& statistics) {
        return std::string("Present ") + statistics.present +
               (statistics.vsync ? "  |  VSync ON" : "  |  VSync OFF");
    }
    // Mirrors every value the overlay draws. Minimap geometry is quantized to the same
    // integer pixels used when drawing, so sub-pixel movement does not force a repaint.
    uint64_t contentSignature(const Frame& f, const RenderStatistics& statistics, bool enabled) const {
        Signature s;
        s.mix(uint64_t(enabled));
        s.mix(uint64_t(w_));
        s.mix(uint64_t(h_));
        s.mix(f.console.open);
        if (f.console.open) {
            s.text(f.console.input);
            s.text(f.console.beforeCursor);
            s.mix(f.console.scroll);
            s.mix(f.console.selectedSuggestion);
            for (const auto& line : f.console.lines) {
                s.text(line.text);
                s.mix(line.error);
            }
            for (const auto& suggestion : f.console.suggestions)
                s.text(suggestion);
        }
        if (!enabled)
            return s.value;
        s.mix(uint64_t(f.debugView));
        s.mix(uint64_t(f.selected));
        s.mix(uint64_t(f.hovered));
        s.mix(uint64_t(f.physicsDebug));
        s.text(f.locomotion);
        s.text(f.message);
        const auto& inspection = f.animationInspection;
        s.mix(inspection.entity);
        s.text(inspection.name);
        s.text(inspection.solver);
        s.mix(inspection.schema.size());
        for (const auto& attribute : inspection.schema) {
            s.text(attribute.key);
            s.text(attribute.label);
            s.text(inspection.values.at(attribute.key));
            s.mix(attribute.options.size());
            for (const auto& option : attribute.options) {
                s.text(option.value);
                s.text(option.label);
            }
        }
        if (f.hovered && f.hovered != f.selected)
            s.text(f.hoveredName);
        s.mix(uint64_t(int64_t(f.player.x * 5.2f)));
        s.mix(uint64_t(int64_t(f.player.z * 5.2f)));
        s.mix(f.path.size());
        for (auto point : f.path) {
            s.mix(uint64_t(int64_t(point.x * 5.2f)));
            s.mix(uint64_t(int64_t(point.z * 5.2f)));
        }
        s.mix(f.mapObstacles.size());
        for (const auto& o : f.mapObstacles) {
            s.mix(uint64_t(int64_t(o.position.x * 5.2f)));
            s.mix(uint64_t(int64_t(o.position.z * 5.2f)));
            s.mix(uint64_t(int64_t(o.size.x * 2.6f)));
            s.mix(uint64_t(int64_t(o.size.z * 2.6f)));
        }
        s.real(statistics.fps);
        s.real(statistics.frameMs);
        s.real(statistics.cpuMs);
        s.real(statistics.gpuMs);
        s.text(presentLabel(statistics));
        return s.value;
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
        signatureValid_ = false;
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
    // Returns true when `output` was rewritten and the caller must upload it to the GPU.
    bool draw(const Frame& f, void* output, const RenderStatistics& statistics, bool enabled) {
        if (!pixels_)
            return false;
        // The physics wireframe tracks live poses, so it cannot be signature-cached.
        const bool live = enabled && f.physicsDebug;
        const uint64_t signature = contentSignature(f, statistics, enabled);
        if (signatureValid_ && signature == signature_ && !live)
            return false;
        signature_ = signature;
        signatureValid_ = true;
        if (!enabled && !f.console.open) {
            std::memset(output, 0, size_t(w_) * h_ * 4);
            return true;
        }
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
            if (f.hovered && f.hovered != f.selected)
                text(w / 2 - 100, h - 91, 14, "CLICK / E    " + f.hoveredName, gold, true);
            rectangle(0, h - 32, w, 32, RGB(12, 23, 24));
            text(28, h - 25, 11,
                 "WASD  Move     CLICK  Walk / interact     SHIFT  Run     CTRL  Crouch     E  Use",
                 RGB(193, 209, 196));
            if (w > 1000)
                text(w - 469, h - 25, 10, "MMB  Orbit   WHEEL  Zoom   F  Follow   F10  Console   ESC  Stop",
                     muted);
            static const char* modes[] = {"LIT",          "ALBEDO",          "NORMALS",
                                          "VIEW DEPTH",   "DIRECT RT (RAW)", "INDIRECT RT (RAW)",
                                          "RAW RADIANCE", "MOTION"};
            int statsY = w > 800 ? 235 : 24;
            rectangle(w - 238, statsY, 210, 117, ink);
            text(w - 224, statsY + 7, 10, modes[std::clamp(f.debugView, 0, 7)], muted);
            std::ostringstream fps, timing, cpu;
            if (statistics.fps >= 0) {
                fps << std::fixed << std::setprecision(1) << statistics.fps << " FPS";
                timing << std::fixed << std::setprecision(2) << "Frame " << statistics.frameMs << " ms   GPU "
                       << statistics.gpuMs << " ms";
                cpu << std::fixed << std::setprecision(2) << "CPU (Render) " << statistics.cpuMs << " ms";
            } else {
                fps << "-- FPS";
                timing << "Measuring frame time...";
                cpu << "CPU (Render) -- ms";
            }
            text(w - 224, statsY + 24, 24, fps.str(), mint, true);
            text(w - 224, statsY + 56, 10, timing.str(), muted);
            text(w - 224, statsY + 74, 10, cpu.str(), muted);
            text(w - 224, statsY + 95, 10, presentLabel(statistics), gold);
            if (f.physicsDebug) {
                rectangle(w - 238, statsY + 122, 210, 42, ink);
                text(w - 224, statsY + 127, 10, "F2  PHYSICS SCENE", mint);
                text(w - 224, statsY + 146, 9, "BLUE Ground  GOLD Solid  PINK Trigger", muted);
            }
            const auto& inspection = f.animationInspection;
            auto inspector = ui::animationInspectorLayout(inspection, w_, h_);
            if (inspector.panel.width) {
                const auto& r = inspector.panel;
                rectangle(r.x, r.y, r.width, r.height, ink);
                rectangle(r.x, r.y, 2, r.height, gold);
                text(r.x + 12, r.y + 10, 12, "ANIMATION  /  " + inspection.name, mint, true);
                text(r.x + 12, r.y + 32, 11, inspection.solver, muted);
                if (!inspection.schema.empty())
                    text(r.x + 12, r.y + 48, 10, "Move to compare", muted);
                for (const auto& control : inspector.controls) {
                    const auto& attribute = inspection.schema[control.attribute];
                    const auto& current = inspection.values.at(attribute.key);
                    auto found =
                        std::find_if(attribute.options.begin(), attribute.options.end(),
                                     [&](const animation::EnumOption& o) { return o.value == current; });
                    auto index = size_t(found - attribute.options.begin());
                    text(control.previous.x, control.previous.y - 20, 11,
                         attribute.label + "  " + std::to_string(index + 1) + " / " +
                             std::to_string(attribute.options.size()),
                         gold);
                    for (const auto& button : {control.previous, control.value, control.next})
                        rectangle(button.x, button.y, button.width, button.height, RGB(37, 68, 61));
                    text(control.previous.x + 9, control.previous.y + 4, 17, "<", mint, true);
                    text(control.next.x + 9, control.next.y + 4, 17, ">", mint, true);
                    text(control.value.x + 10, control.value.y + 6, 13, found->label, RGB(239, 240, 222));
                }
                if (inspection.schema.empty())
                    text(r.x + 12, r.y + 48, 10, "No editable attributes", muted);
            }
        }
        console(f.console);
        GdiFlush();
        // One 32-bit load and one 32-bit store per pixel. `output` is host-visible device
        // memory, which is write-combined on discrete GPUs: byte-sized stores there defeat
        // write combining and cost roughly an order of magnitude more than whole words.
        const auto* source = reinterpret_cast<const uint32_t*>(pixels_);
        auto* destination = static_cast<uint32_t*>(output);
        for (size_t i = 0, n = size_t(w_) * h_; i < n; i++) {
            const uint32_t bgr = source[i] & 0x00ffffffu; // GDI DIB order is B, G, R, unused
            const uint32_t alpha = bgr ? 245u : 0u;
            destination[i] = ((bgr & 0x00ff0000u) >> 16) | (bgr & 0x0000ff00u) | ((bgr & 0x000000ffu) << 16) |
                             (alpha << 24);
        }
        return true;
    }
};
} // namespace afterlight
