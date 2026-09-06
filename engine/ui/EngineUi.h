#pragma once
#include "uiCore/UiCore.h"
#include "core/Types.h"
#include <RmlUi/Core.h>
namespace afterlight {
struct RenderStatistics;
namespace ui {
// Host-owned diagnostics only. Gameplay documents and events belong to the project.
class EngineUi {
    UiCore& ui_;
    Rml::ElementDocument *tools_, *console_;
    std::map<std::pair<Rml::ElementDocument*, std::string>, std::string> content_;
    void rml(Rml::ElementDocument*, const char* id, const std::string&);

  public:
    explicit EngineUi(UiCore&);
    ~EngineUi();
    void sync(const Frame&, const RenderStatistics&);
    std::shared_ptr<const UiFrame> snapshot(const Frame&);
};
} // namespace ui
} // namespace afterlight
