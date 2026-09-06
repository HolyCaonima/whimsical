#pragma once
#include "uiCore/UiCore.h"
#include "core/Types.h"
#include <optional>
#include <RmlUi/Core.h>
namespace afterlight {
class World;
struct RenderStatistics;
namespace ui {
// Engine tools consume the same DOM and input path that project scripts use.
class EngineUi : Rml::EventListener {
    UiCore& ui_;
    World& world_;
    Rml::ElementDocument *tools_, *console_;
    std::string inspectionSignature_;
    void ProcessEvent(Rml::Event&) override;
    std::map<std::pair<Rml::ElementDocument*, std::string>, std::string> content_;
    void rml(Rml::ElementDocument*, const char* id, const std::string&);

  public:
    EngineUi(UiCore&, World&);
    ~EngineUi();
    void sync(const Frame&, const RenderStatistics&);
    Rml::ElementDocument& tools() {
        return *tools_;
    }
    std::shared_ptr<const UiFrame> snapshot(const Frame&);
};
} // namespace ui
} // namespace afterlight
