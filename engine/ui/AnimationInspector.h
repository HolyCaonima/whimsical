#pragma once
#include "core/Types.h"

namespace afterlight {
class World;
namespace ui {
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};
struct EnumControl {
    size_t attribute = 0;
    Rect previous, value, next;
};
struct AnimationInspectorLayout {
    Rect panel;
    std::vector<EnumControl> controls;
};
// Shared by hit testing and HUD rendering; neither contains solver-specific keys.
AnimationInspectorLayout animationInspectorLayout(const AnimationInspection&, uint32_t width,
                                                  uint32_t height);
bool animationInspectorInput(World&, const Input&);
} // namespace ui
} // namespace afterlight
