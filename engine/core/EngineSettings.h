#pragma once
#include "ConsoleRegistry.h"
#include "Types.h"
namespace whimsical {
// Domain registration and the Frame boundary are separate from the generic registry/UI.
class EngineSettings {
    ConsoleRegistry& vars_;

  public:
    explicit EngineSettings(ConsoleRegistry& vars, bool validationDefault);
    void decorate(Frame&) const;
    bool hud() const {
        return vars_.get<bool>("r.Hud");
    }
    int debugView() const {
        return vars_.get<int>("r.DebugView");
    }
    bool physicsDebug() const {
        return vars_.get<bool>("p.DebugDraw");
    }
    double timeScale() const {
        return vars_.get<double>("t.TimeScale");
    }
};
} // namespace whimsical
