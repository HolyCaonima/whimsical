#include "EngineUi.h"
#include "core/World.h"
#include "render/Renderer.h"
#include <iomanip>
#include <sstream>
#include <cstring>

namespace afterlight::ui {
EngineUi::EngineUi(UiCore& ui, World& world) : ui_(ui), world_(world) {
    tools_ = ui_.loadDocument("/Engine/UI/tools.rml");
    console_ = ui_.loadDocument("/Engine/UI/console.rml");
    tools_->AddEventListener("click", this);
    tools_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
}
EngineUi::~EngineUi() {
    tools_->RemoveEventListener("click", this);
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
void EngineUi::ProcessEvent(Rml::Event& event) {
    auto* target = event.GetTargetElement();
    for (; target && target != tools_; target = target->GetParentNode()) {
        auto* key = target->GetAttribute("data-attribute");
        if (!key)
            continue;
        auto inspection = world_.inspectAnimation(world_.selected);
        auto name = key->Get<Rml::String>();
        for (const auto& attribute : inspection.schema)
            if (attribute.key == name) {
                const auto& current = inspection.values.at(name);
                auto found = std::find_if(attribute.options.begin(), attribute.options.end(),
                                          [&](const auto& option) { return option.value == current; });
                int count = int(attribute.options.size());
                int direction = target->GetAttribute<int>("data-direction", 1);
                auto index = (int(found - attribute.options.begin()) + direction + count) % count;
                world_.setAnimationAttribute(inspection.entity, name, attribute.options[index].value);
            }
        event.StopPropagation();
        return;
    }
}
void EngineUi::sync(const Frame& frame, const RenderStatistics& statistics) {
    ui_.resize(frame.input.width, frame.input.height);
    if (frame.hudEnabled && !tools_->IsVisible())
        tools_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    else if (!frame.hudEnabled && tools_->IsVisible())
        tools_->Hide();
    auto inspection = frame.animationInspection;
    auto* panel = tools_->GetElementById("animation");
    panel->SetClass("empty", !inspection.entity);
    std::string controls;
    for (size_t i = 0; i < inspection.schema.size(); ++i) {
        const auto& attribute = inspection.schema[i];
        const auto& current = inspection.values.at(attribute.key);
        auto found = std::find_if(attribute.options.begin(), attribute.options.end(),
                                  [&](const auto& option) { return option.value == current; });
        controls += "<div class='attribute'><div class='label'>" + UiCore::escape(attribute.label) + " " +
                    std::to_string(found - attribute.options.begin() + 1) + " / " +
                    std::to_string(attribute.options.size()) + "</div><button id='previous-" +
                    std::to_string(i) + "' data-attribute='" + UiCore::escape(attribute.key) +
                    "' data-direction='-1'>&lt;</button><span class='value'>" + UiCore::escape(found->label) +
                    "</span><button id='next-" + std::to_string(i) + "' data-attribute='" +
                    UiCore::escape(attribute.key) + "' data-direction='1'>&gt;</button></div>";
    }
    if (controls != inspectionSignature_) {
        rml(tools_, "attributes", controls);
        inspectionSignature_ = controls;
    }
    rml(tools_, "animation-title", "ANIMATION / " + UiCore::escape(inspection.name));
    rml(tools_, "solver", UiCore::escape(inspection.solver));
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
    // The minimap is ordinary RML geometry; it scales and clips through the same backend as text.
    std::string map;
    auto rect = [&](float fx, float fy, float fw, float fh, const char* color) {
        int x = int(fx), y = int(fy), w = int(fw), h = int(fh);
        map += "<div style='position:absolute;left:" + std::to_string(x) + "px;top:" + std::to_string(y) +
               "px;width:" + std::to_string(w) + "px;height:" + std::to_string(h) +
               "px;background-color:" + color + ";'/>";
    };
    for (const auto& o : frame.mapObstacles)
        rect(98 + o.position.x * 5.2f - o.size.x * 2.6f, 75 + o.position.z * 5.2f - o.size.z * 2.6f,
             std::max(2.f, o.size.x * 5.2f), std::max(2.f, o.size.z * 5.2f), "#495d54");
    rect(95 + frame.player.x * 5.2f, 72 + frame.player.z * 5.2f, 6, 6, "#75dabb");
    for (auto point : frame.path)
        rect(97 + point.x * 5.2f, 74 + point.z * 5.2f, 3, 3, "#b8ab7a");
    rml(tools_, "map", map);
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
    if (frame.hudEnabled && frame.physicsDebug) {
        static uint64_t id = uint64_t(1) << 63;
        auto geometry = std::make_shared<Geometry>();
        geometry->id = ++id;
        auto vp = frame.camera.projection(float(result->width) / result->height) * frame.camera.view();
        for (const auto& line : frame.physicsLines) {
            vec4 a = vp * vec4(line.a, 1), b = vp * vec4(line.b, 1);
            if (a.w <= .1f || b.w <= .1f)
                continue;
            vec2 p = (vec2(a) / a.w * .5f + .5f) * vec2(result->width, result->height);
            vec2 q = (vec2(b) / b.w * .5f + .5f) * vec2(result->width, result->height);
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
            result->draws.insert(result->draws.begin(), {geometry,
                                                         nullptr,
                                                         {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
                                                         0,
                                                         0,
                                                         {0, 0, result->width, result->height}});
    }
    return result;
}
} // namespace afterlight::ui
