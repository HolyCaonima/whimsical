#include "EngineUi.h"
#include "render/Renderer.h"
#include <iomanip>
#include <sstream>

namespace afterlight::ui {
EngineUi::EngineUi(UiCore& ui) : ui_(ui) {
    tools_ = ui_.loadDocument("/Engine/UI/tools.rml");
    console_ = ui_.loadDocument("/Engine/UI/console.rml");
}
EngineUi::~EngineUi() {
    tools_->Close();
    console_->Close();
    ui_.context().Update();
}
void EngineUi::rml(Rml::ElementDocument* doc, const char* id, const std::string& value) {
    auto& previous = content_[{doc, id}];
    if (previous != value) {
        doc->GetElementById(id)->SetInnerRML(value);
        previous = value;
    }
}
void EngineUi::sync(const Frame& frame, const RenderStatistics& statistics) {
    ui_.resize(frame.input.width, frame.input.height);
    if (frame.statsEnabled && !tools_->IsVisible())
        tools_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    else if (!frame.statsEnabled && tools_->IsVisible())
        tools_->Hide();
    static const char* views[] = {"LIT",          "ALBEDO",          "NORMALS",
                                  "VIEW DEPTH",   "DIRECT RT (RAW)", "INDIRECT RT (RAW)",
                                  "RAW RADIANCE", "MOTION"};
    rml(tools_, "view", views[std::clamp(frame.debugView, 0, 7)]);
    std::ostringstream fps, timing;
    if (statistics.fps >= 0) {
        fps << std::fixed << std::setprecision(1) << statistics.fps << " FPS";
        timing << std::fixed << std::setprecision(2) << "Frame " << statistics.frameMs << " ms | GPU "
               << statistics.gpuMs << " ms<br/>CPU (Render) " << statistics.cpuMs << " ms";
    } else {
        fps << "-- FPS";
        timing << "Measuring frame time...";
    }
    rml(tools_, "fps", fps.str());
    rml(tools_, "timing", timing.str());
    rml(tools_, "present",
        std::string("Present ") + statistics.present + (statistics.vsync ? " | VSync ON" : " | VSync OFF"));
    tools_->GetElementById("physics-label")->SetClass("enabled", frame.physicsDebug);
    if (frame.console.open) {
        if (!console_->IsVisible())
            console_->Show(Rml::ModalFlag::Modal);
        std::string lines, suggestions;
        for (const auto& line : frame.console.lines)
            lines += "<div class='" + std::string(line.error ? "error" : "line") + "'>" +
                     UiCore::escape(line.text) + "</div>";
        for (size_t i = 0; i < frame.console.suggestions.size(); ++i)
            suggestions +=
                "<div class='" +
                std::string(int(i) == frame.console.selectedSuggestion ? "selected" : "suggestion") + "'>" +
                UiCore::escape(frame.console.suggestions[i]) + "</div>";
        rml(console_, "log-content", lines);
        rml(console_, "suggestions", suggestions);
        rml(console_, "input",
            "&gt; " + UiCore::escape(frame.console.beforeCursor) + "<span id='caret' class='caret'>|</span>" +
                UiCore::escape(frame.console.input.substr(frame.console.beforeCursor.size())));
        rml(console_, "scroll",
            frame.console.scroll ? "SCROLL +" + std::to_string(frame.console.scroll) : "");
    } else if (console_->IsVisible())
        console_->Hide();
}
std::shared_ptr<const UiFrame> EngineUi::snapshot(const Frame& frame) {
    if (console_->IsVisible()) {
        ui_.context().Update();
        auto* input = console_->GetElementById("input");
        auto* caret = console_->GetElementById("caret");
        const float cursor = caret->GetAbsoluteLeft() - input->GetAbsoluteLeft() + input->GetScrollLeft();
        input->SetScrollLeft(std::max(0.f, cursor - input->GetClientWidth() + 20));
    }
    auto result = std::make_shared<UiFrame>(*ui_.snapshot());
    // Physics wireframes are debug geometry, not widgets. They share the GPU overlay pass.
    if (frame.physicsDebug) {
        auto rect = frame.viewport.fit(result->width, result->height);
        if (!rect.width || !rect.height)
            return result;
        static uint64_t id = uint64_t(1) << 63;
        auto geometry = std::make_shared<Geometry>();
        geometry->id = ++id;
        auto vp = frame.camera.projection(float(rect.width) / rect.height) * frame.camera.view();
        for (const auto& line : frame.physicsLines) {
            vec4 a = vp * vec4(line.a, 1), b = vp * vec4(line.b, 1);
            if (a.w <= .1f || b.w <= .1f)
                continue;
            vec2 p = vec2(rect.x, rect.y) + (vec2(a) / a.w * .5f + .5f) * vec2(rect.width, rect.height);
            vec2 q = vec2(rect.x, rect.y) + (vec2(b) / b.w * .5f + .5f) * vec2(rect.width, rect.height);
            if (glm::length(q - p) < .01f)
                continue;
            vec2 n = glm::normalize(vec2(-(q - p).y, (q - p).x)) * .5f;
            uint32_t color = uint32_t(line.color.r * 255) | (uint32_t(line.color.g * 255) << 8) |
                             (uint32_t(line.color.b * 255) << 16) | 0xff000000u;
            int first = int(geometry->vertices.size());
            for (auto point : {p + n, p - n, q - n, q + n})
                geometry->vertices.push_back({point.x, point.y, color, 0, 0});
            for (int index : {0, 1, 2, 0, 2, 3})
                geometry->indices.push_back(first + index);
        }
        if (!geometry->indices.empty())
            result->draws.insert(result->draws.begin(),
                                 {geometry,
                                  nullptr,
                                  {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
                                  0,
                                  0,
                                  {int(rect.x), int(rect.y), int(rect.width), int(rect.height)}});
    }
    return result;
}
} // namespace afterlight::ui
