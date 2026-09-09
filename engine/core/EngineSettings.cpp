#include "EngineSettings.h"
namespace whimsical {
EngineSettings::EngineSettings(ConsoleRegistry& v, bool validationDefault) : vars_(v) {
    v.variable("r.Exposure", 1.15, "Render tonemapping exposure multiplier", CVarArchive, CVarRange{.05, 8});
    v.variable("r.Hud", true, "Show gameplay HUD; the console remains accessible", CVarArchive);
    v.variable("r.Stats", false, "Show engine performance statistics independently of gameplay HUD", CVarArchive);
    v.variable("r.DebugView", 0,
               "Render view: 0 lit, 1 albedo, 2 normals, 3 depth, 4 direct, 5 indirect, 6 raw, 7 motion",
               CVarNone, CVarRange{0, 7});
    v.variable("r.FullUpload", false, "Force full render scene uploads for profiling", CVarNone);
    v.variable("r.DIHistoryConfidence", true, "RTXDI visibility feedback to reservoir and NRD histories", CVarNone);
    v.variable("r.Present", std::string("fifo"), "Vulkan present mode, applied when creating the renderer",
               CVarArchive | CVarRestart, {}, {"fifo", "mailbox", "immediate"});
    v.variable("r.Validation", validationDefault, "Vulkan validation at RenderCore device creation", CVarRestart);
    v.variable("p.DebugDraw", false, "Draw physics colliders independently of gameplay HUD", CVarNone);
    v.variable("t.TimeScale", 1.0,
               "Gameplay time multiplier; 0 pauses gameplay while console and rendering continue", CVarNone,
               CVarRange{0, 4});
    v.variable("sys.Engine", std::string("Whimsical"), "Engine identifier", CVarReadOnly);
}
void EngineSettings::decorate(Frame& f) const {
    f.hudEnabled = hud();
    f.statsEnabled = vars_.get<bool>("r.Stats");
    f.exposure = float(vars_.get<double>("r.Exposure"));
    f.forceFullUpload = vars_.get<bool>("r.FullUpload");
    f.diHistoryConfidence = vars_.get<bool>("r.DIHistoryConfidence");
}
} // namespace whimsical
